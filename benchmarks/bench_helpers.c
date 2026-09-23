/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_sampling.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define BENCH_NOINLINE __declspec(noinline)
#define BENCH_BARRIER(pointer) do { (void)(pointer); _ReadWriteBarrier(); } while(0)
#elif defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#define BENCH_BARRIER(pointer) __asm__ __volatile__("" : : "r"(pointer) : "memory")
#else
#define BENCH_NOINLINE
#define BENCH_BARRIER(pointer) ((void)(pointer))
#endif

enum { INPUT_COUNT = 16, MAX_LENGTH = 4096, REPEATS = 5 };
enum operation { ASCII_COPY, ASCII_INPLACE, HEX_LOWER, HEX_UPPER };
typedef simdurl_result (*helper_function)(const char *, size_t, char *);
typedef char input_buffers[INPUT_COUNT][MAX_LENGTH + INPUT_COUNT + 2];
typedef char output_buffers[INPUT_COUNT][2 * MAX_LENGTH + INPUT_COUNT + 2];
static volatile uint64_t checksum;

/* Independent, ordinary portable C comparators. Optimization and compiler
 * vectorization stay enabled. Unlike the API, these assume valid arguments. */
static inline simdurl_result reference_ascii(const char *input, size_t length,
                                             char *output)
{
  simdurl_result result = { SIMDURL_OK, length };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char byte = (unsigned char)input[i];
    output[i] = (char)(byte >= 'A' && byte <= 'Z' ? byte + 32 : byte);
  }
  return result;
}

static inline simdurl_result reference_hex(const char *input, size_t length,
                                           char *output, unsigned int upper)
{
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  simdurl_result result = { SIMDURL_OK, 2 * length };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned int byte = (unsigned char)input[i];
    output[2 * i] = digits[byte >> 4];
    output[2 * i + 1] = digits[byte & 15];
  }
  return result;
}

static BENCH_NOINLINE simdurl_result portable_ascii(const char *s, size_t n,
                                                    char *d)
{
  return reference_ascii(s, n, d);
}

static BENCH_NOINLINE simdurl_result library_ascii(const char *s, size_t n,
                                                   char *d)
{
  return simdurl_ascii_lower(s, n, d, n);
}

static BENCH_NOINLINE simdurl_result portable_hex_lower(const char *s, size_t n,
                                                        char *d)
{
  return reference_hex(s, n, d, 0);
}

static BENCH_NOINLINE simdurl_result portable_hex_upper(const char *s, size_t n,
                                                        char *d)
{
  return reference_hex(s, n, d, 1);
}

static BENCH_NOINLINE simdurl_result library_hex_lower(const char *s, size_t n,
                                                       char *d)
{
  return simdurl_hex_encode(s, n, d, 2 * n, SIMDURL_HEX_LOWER);
}

static BENCH_NOINLINE simdurl_result library_hex_upper(const char *s, size_t n,
                                                       char *d)
{
  return simdurl_hex_encode(s, n, d, 2 * n, SIMDURL_HEX_UPPER);
}

/* Both sides receive the same compile-time length and case. This matters for
 * short digests where unrolling and API call overhead can dominate. */
#define FIXED_HEX_WRAPPERS(size) \
  static BENCH_NOINLINE simdurl_result portable_hex_lower_##size( \
    const char *s, size_t n, char *d) \
  { (void)n; return reference_hex(s, size, d, 0); } \
  static BENCH_NOINLINE simdurl_result portable_hex_upper_##size( \
    const char *s, size_t n, char *d) \
  { (void)n; return reference_hex(s, size, d, 1); } \
  static BENCH_NOINLINE simdurl_result library_hex_lower_##size( \
    const char *s, size_t n, char *d) \
  { (void)n; return simdurl_hex_encode(s, size, d, 2 * size, SIMDURL_HEX_LOWER); } \
  static BENCH_NOINLINE simdurl_result library_hex_upper_##size( \
    const char *s, size_t n, char *d) \
  { (void)n; return simdurl_hex_encode(s, size, d, 2 * size, SIMDURL_HEX_UPPER); }

FIXED_HEX_WRAPPERS(16)
FIXED_HEX_WRAPPERS(20)
FIXED_HEX_WRAPPERS(32)
FIXED_HEX_WRAPPERS(64)
#undef FIXED_HEX_WRAPPERS

struct fixed_case {
  size_t length;
  helper_function functions[2][2];
};

#define FIXED_CASE(size) { size, { \
  { portable_hex_lower_##size, library_hex_lower_##size }, \
  { portable_hex_upper_##size, library_hex_upper_##size } } }
static const struct fixed_case fixed_cases[] = {
  FIXED_CASE(16), FIXED_CASE(20), FIXED_CASE(32), FIXED_CASE(64)
};
#undef FIXED_CASE

static void fill_inputs(input_buffers input, size_t length,
                         unsigned int pattern, enum operation operation)
{
  const char *text = pattern ? "api.example.test:443/request_id-0123456789" :
                               "Host.Example.COM:443/X-Request-Id";
  size_t row, i, text_length = strlen(text);
  for(row = 0; row < INPUT_COUNT; ++row) {
    char *data = input[row] + row + 1;
    for(i = 0; i < length; ++i) {
      if(operation >= HEX_LOWER)
        data[i] = (char)((i * 73 + row * 29) & 255);
      else if(pattern == 2)
        data[i] = (char)(128 + (i + row) % 128);
      else
        data[i] = text[(i + row) % text_length];
    }
  }
}

static void prepare_outputs(input_buffers input, output_buffers output,
                              size_t length, enum operation operation)
{
  size_t row;
  memset(output, 0xa5, sizeof(output_buffers));
  if(operation == ASCII_INPLACE)
    for(row = 0; row < INPUT_COUNT; ++row)
      reference_ascii(input[row] + row + 1, length, output[row] + row + 1);
}

static int validate_outputs(input_buffers input, output_buffers output,
                             size_t length, enum operation operation)
{
  char expected[2 * MAX_LENGTH];
  size_t row, written = operation >= HEX_LOWER ? 2 * length : length;
  for(row = 0; row < INPUT_COUNT; ++row) {
    const char *source = input[row] + row + 1;
    const char *destination = output[row] + row + 1;
    if(operation >= HEX_LOWER)
      reference_hex(source, length, expected, operation == HEX_UPPER);
    else
      reference_ascii(source, length, expected);
    if(memcmp(destination, expected, written) ||
       (unsigned char)destination[-1] != 0xa5 ||
       (unsigned char)destination[written] != 0xa5)
      return 1;
  }
  return 0;
}

static BENCH_NOINLINE double measure(helper_function function,
  input_buffers input, output_buffers output, size_t length,
  enum operation operation, size_t iterations, uint64_t *elapsed_ns)
{
  uint64_t sum = 0;
  size_t i;
  clock_t start = clock(), end;
  for(i = 0; i < iterations; ++i) {
    size_t row = i % INPUT_COUNT;
    char *destination = output[row] + row + 1;
    const char *source = operation == ASCII_INPLACE ? destination :
                          input[row] + row + 1;
    simdurl_result result;
    BENCH_BARRIER(source);
    result = function(source, length, destination);
    BENCH_BARRIER(destination);
    sum += (unsigned int)result.status + result.written;
    if(result.written)
      sum += (unsigned char)destination[0] +
             (unsigned char)destination[result.written - 1];
  }
  end = clock();
  checksum += sum;
  if(bench_elapsed_ns(start, end, elapsed_ns))
    return -1.0;
  return (double)*elapsed_ns / (double)iterations;
}

struct helper_batch {
  helper_function function;
  char (*input)[MAX_LENGTH + INPUT_COUNT + 2];
  char (*output)[2 * MAX_LENGTH + INPUT_COUNT + 2];
  size_t length;
  enum operation operation;
};

static int measure_batch(void *opaque, size_t iterations, uint64_t *elapsed_ns)
{
  struct helper_batch *context = opaque;
  size_t row, written = context->operation >= HEX_LOWER ?
                         2 * context->length : context->length;
  prepare_outputs(context->input, context->output, context->length,
                  context->operation);
  /* Initialize all rows outside timing, including tiny calibration seeds. */
  for(row = 0; row < INPUT_COUNT; ++row) {
    char *destination = context->output[row] + row + 1;
    simdurl_result result = context->function(
      context->operation == ASCII_INPLACE ? destination :
        context->input[row] + row + 1, context->length, destination);
    if(result.status != SIMDURL_OK || result.written != written)
      return 1;
  }
  if(measure(context->function, context->input, context->output, context->length,
             context->operation, iterations, elapsed_ns) < 0 ||
     validate_outputs(context->input, context->output, context->length,
                      context->operation))
    return 1;
  return 0;
}

static int run_case(size_t length, unsigned int pattern,
                    enum operation operation, const struct fixed_case *fixed,
                    size_t iterations, int core, int calibrated)
{
  static const char *const operations[] = {
    "ascii_copy", "ascii_inplace_already_lowered", "hex_lower", "hex_upper"
  };
  static const char *const patterns[] = {
    "mixed_ascii", "unchanged_ascii", "high_bytes"
  };
  static const char *const variants[] = { "portable_C_comparator", "simdurl" };
  static const helper_function runtime_functions[4][2] = {
    { portable_ascii, library_ascii }, { portable_ascii, library_ascii },
    { portable_hex_lower, library_hex_lower },
    { portable_hex_upper, library_hex_upper }
  };
  input_buffers input;
  output_buffers output;
  const helper_function *functions = fixed ?
    fixed->functions[operation == HEX_UPPER] : runtime_functions[operation];
  size_t row, written = operation >= HEX_LOWER ? 2 * length : length;
  unsigned int repeat, order, variant;
  uint64_t elapsed_ns;
  fill_inputs(input, length, pattern, operation);
  for(variant = 0; variant < 2; ++variant) {
    prepare_outputs(input, output, length, operation);
    for(row = 0; row < INPUT_COUNT; ++row) {
      char *destination = output[row] + row + 1;
      const char *source = operation == ASCII_INPLACE ? destination :
                            input[row] + row + 1;
      simdurl_result result = functions[variant](source, length, destination);
      if(result.status != SIMDURL_OK || result.written != written)
        return 1;
    }
    if(validate_outputs(input, output, length, operation) ||
       (!calibrated && (!core || variant) &&
        measure(functions[variant], input, output, length, operation, 1000,
                &elapsed_ns) < 0))
      return 1;
  }
  if(calibrated) {
    char name[160];
    struct helper_batch context = {
      functions[1], input, output, length, operation
    };
    snprintf(name, sizeof(name), "helpers/%s/%s/%lu/%s/simdurl/automatic",
             operations[operation], operation >= HEX_LOWER ? "binary" :
               patterns[pattern], (unsigned long)length,
             fixed ? "fixed" : "runtime");
    return bench_sample_case(name, iterations, measure_batch, &context);
  }
  for(repeat = 0; repeat < REPEATS; ++repeat) {
    for(order = 0; order < 2; ++order) {
      double ns;
      variant = (order + repeat) % 2;
      if(core && !variant)
        continue;
      prepare_outputs(input, output, length, operation);
      /* Initialize every output row even when iterations < INPUT_COUNT. */
      for(row = 0; row < INPUT_COUNT; ++row) {
        char *destination = output[row] + row + 1;
        functions[variant](operation == ASCII_INPLACE ? destination :
                             input[row] + row + 1, length, destination);
      }
      ns = measure(functions[variant], input, output, length, operation,
                   iterations, &elapsed_ns);
      if(ns < 0 || validate_outputs(input, output, length, operation))
        return 1;
      printf("%s,%s,%lu,%s,%s,%u,%.3f\n", operations[operation],
             operation >= HEX_LOWER ? "binary" : patterns[pattern],
             (unsigned long)length, fixed ? "fixed" : "runtime",
             variants[variant], repeat + 1, ns);
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  static const size_t lengths[] = {
    0, 1, 8, 15, 16, 17, 20, 31, 32, 33, 48, 63, 64, 65, 128, 512, 4096
  };
  size_t iterations = 100000, length_index;
  unsigned int pattern, operation;
  int core = argc == 3;
  int calibrated = core && !strcmp(argv[2], "--calibrated");
  if(argc > 3 || (core && !calibrated && strcmp(argv[2], "--core"))) {
    fprintf(stderr, "Usage: %s [iterations_per_sample] [--core|--calibrated]\n", argv[0]);
    return 1;
  }
  if(argc >= 2) {
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(argv[1], &end, 10);
    if(errno || end == argv[1] || *end || argv[1][0] == '-' || !parsed ||
       parsed > SIZE_MAX) {
      fputs("iterations_per_sample must be a positive integer\n", stderr);
      return 1;
    }
    iterations = (size_t)parsed;
  }
#if defined(SIMDURL_BENCH_COMPILED) || defined(SIMDURL_DISABLE_SIMD)
  if(calibrated) {
    fputs("Calibrated sampling requires the automatic backend binary\n", stderr);
    return 1;
  }
#endif
  if(calibrated)
    bench_sampling_header();
  else {
#ifdef SIMDURL_BENCH_COMPILED
    puts("# simdurl: compiled library; configured library CPU selection applies");
#elif defined(SIMDURL_DISABLE_SIMD)
    puts("# simdurl: portable C header-only; compiler-generated SIMD permitted");
#else
    puts("# simdurl: automatic CPU selection (header-only)");
#endif
    puts("# comparators: independent portable C; optimization and vectorization enabled");
    puts("# comparators assume valid arguments; simdurl includes API argument checks");
    puts("# both variants use noinline wrappers; compiled simdurl retains a separate API boundary");
    puts("# fixed hex wrappers expose constant lengths and case to both variants");
    puts("# inplace inputs are already lowercased before timing; no input-reset cost included");
    puts("# checksums, output sampling and call overhead are included in both timings");
    printf("# %lu iterations/sample; %u %ssamples; CPU time; ns/operation\n",
           (unsigned long)iterations, (unsigned int)REPEATS,
           core ? "" : "alternating ");
    puts("operation,pattern,bytes,length_kind,variant,sample,ns_per_op");
  }
  for(length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]);
      ++length_index)
    for(operation = ASCII_COPY; operation <= HEX_UPPER; ++operation)
      for(pattern = 0; pattern < (operation >= HEX_LOWER ? 1U : 3U); ++pattern) {
        if(!lengths[length_index] && pattern)
          continue;
        if(core && !(
           (operation == ASCII_COPY && !pattern && lengths[length_index] == 128) ||
           (operation == HEX_LOWER && lengths[length_index] == 4096)))
          continue;
        if(run_case(lengths[length_index], pattern, (enum operation)operation,
                    NULL, iterations, core, calibrated))
          goto failure;
      }
  for(length_index = 0;
      length_index < sizeof(fixed_cases) / sizeof(fixed_cases[0]); ++length_index)
    for(operation = HEX_LOWER; operation <= HEX_UPPER; ++operation) {
      if(core && !(operation == HEX_LOWER && fixed_cases[length_index].length == 32))
        continue;
      if(run_case(fixed_cases[length_index].length, 0, (enum operation)operation,
                  &fixed_cases[length_index], iterations, core, calibrated))
        goto failure;
    }
  printf("# checksum: %" PRIu64 "\n", checksum);
  return 0;
failure:
  fputs("Benchmark output validation or timer failed\n", stderr);
  return 1;
}
