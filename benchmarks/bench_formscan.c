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

enum { MAX_LENGTH = 16384, SAMPLES = 5 };
enum pattern {
  LITERAL, PLUS_SHORT, PLUS_LONG, PERCENT_SHORT, PERCENT_LONG, MIXED
};
typedef simdurl_result (*decode_function)(const char *, size_t, char *);
static volatile uint64_t checksum;

static BENCH_NOINLINE simdurl_result decode_uri(const char *s, size_t n, char *d)
{
  return simdurl_decode(s, n, d, n, SIMDURL_URI);
}

static BENCH_NOINLINE simdurl_result decode_form(const char *s, size_t n, char *d)
{
  return simdurl_decode(s, n, d, n, SIMDURL_FORM);
}

static void fill_input(char *input, size_t length, enum pattern pattern)
{
  static const char mixed[] = "alpha+beta%20gamma%2Bdelta%zz+omega%";
  size_t i, run, period;
  for(i = 0; i < length; ++i)
    input[i] = pattern == MIXED ? mixed[i % (sizeof(mixed) - 1)] :
                                 (char)('a' + i % 26);
  if(pattern == LITERAL || pattern == MIXED)
    return;
  run = (pattern == PLUS_LONG || pattern == PERCENT_LONG) ? 256 : 1;
  period = run + ((pattern == PLUS_SHORT || pattern == PLUS_LONG) ? 1 : 3);
  for(i = run; i < length; i += period) {
    if(pattern == PLUS_SHORT || pattern == PLUS_LONG)
      input[i] = '+';
    else {
      size_t count = length - i < 3 ? length - i : 3;
      memcpy(input + i, "%20", count);
    }
  }
}

static int hex_value(unsigned char c)
{
  if(c >= '0' && c <= '9')
    return c - '0';
  if(c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if(c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Independent byte-wise oracle, used only for correctness outside timing. */
static size_t reference_decode(const char *input, size_t length, char *output,
                                unsigned int form)
{
  size_t i = 0, written = 0;
  while(i < length) {
    unsigned char byte = (unsigned char)input[i++];
    if(byte == '%' && length - i >= 2) {
      int high = hex_value((unsigned char)input[i]);
      int low = hex_value((unsigned char)input[i + 1]);
      if(high >= 0 && low >= 0) {
        output[written++] = (char)(high * 16 + low);
        i += 2;
        continue;
      }
    }
    output[written++] = form && byte == '+' ? ' ' : (char)byte;
  }
  return written;
}

static int run_case(size_t length, enum pattern pattern, unsigned int form,
                    size_t base_iterations)
{
  static const char *const names[] = {
    "literal", "plus_short", "plus_long", "percent_short", "percent_long", "mixed"
  };
  char input_storage[MAX_LENGTH + 2], output_storage[MAX_LENGTH + 2];
  char expected[MAX_LENGTH];
  char *input = input_storage + 1, *output = output_storage + 1;
  decode_function decode = form ? decode_form : decode_uri;
  size_t expected_length, i, iterations = base_iterations / ((length + 63) / 64);
  unsigned int sample;
  simdurl_result result;
  if(!iterations)
    iterations = 1;
  fill_input(input, length, pattern);
  expected_length = reference_decode(input, length, expected, form);
  memset(output_storage, 0xa5, sizeof(output_storage));
  result = decode(input, length, output);
  if(result.status != SIMDURL_OK || result.written != expected_length ||
     memcmp(output, expected, expected_length))
    return 1;
  for(i = 0; i < 8; ++i) {
    BENCH_BARRIER(input);
    result = decode(input, length, output);
    BENCH_BARRIER(output);
    checksum += result.written;
  }
  for(sample = 0; sample < SAMPLES; ++sample) {
    uint64_t sum = 0;
    clock_t start = clock(), end;
    double ns;
    for(i = 0; i < iterations; ++i) {
      BENCH_BARRIER(input);
      result = decode(input, length, output);
      BENCH_BARRIER(output);
      if(result.status != SIMDURL_OK || result.written != expected_length)
        return 1;
      sum += result.written + (unsigned char)output[0] +
             (unsigned char)output[expected_length - 1];
    }
    end = clock();
    checksum += sum;
    if(start == (clock_t)-1 || end == (clock_t)-1 || end < start ||
       memcmp(output, expected, expected_length) ||
       (unsigned char)output[-1] != 0xa5 ||
       (unsigned char)output[length] != 0xa5)
      return 1;
    ns = (double)(end - start) * 1e9 / (double)CLOCKS_PER_SEC /
         (double)iterations;
    printf("%s,%s,%lu,%lu,%u,%.3f,%.6f\n",
           form ? "form" : "URI", names[pattern], (unsigned long)length,
           (unsigned long)iterations, sample + 1, ns, ns / (double)length);
  }
  return 0;
}

int main(int argc, char **argv)
{
  static const size_t lengths[] = { 16, 64, 128, 512, 4096, 16384 };
  size_t iterations = 20000, index;
  unsigned int form, pattern;
  if(argc > 2) {
    fprintf(stderr, "Usage: %s [iterations_at_64_bytes]\n", argv[0]);
    return 1;
  }
  if(argc == 2) {
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(argv[1], &end, 10);
    if(errno || end == argv[1] || *end || argv[1][0] == '-' ||
       !parsed || parsed > SIZE_MAX) {
      fputs("iterations_at_64_bytes must be a positive integer\n", stderr);
      return 1;
    }
    iterations = (size_t)parsed;
  }
#ifdef SIMDURL_BENCH_COMPILED
  puts("# compiled library; configured backend selection applies");
#elif defined(SIMDURL_DISABLE_SIMD)
  puts("# portable C/libc; compiler-generated and libc SIMD remain enabled");
#else
  puts("# automatic CPU selection (header-only)");
#endif
  puts("# fixed mode/noinline wrappers; disjoint buffers; CPU ns/operation");
  puts("# long runs contain 256 literal bytes; short runs contain one");
  puts("# iterations decrease with input length; five samples after warmup");
  puts("# input/output barriers, status checks and output sampling are included");
  puts("mode,pattern,bytes,iterations,sample,ns_per_op,ns_per_byte");
  for(index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index)
    for(pattern = LITERAL; pattern <= MIXED; ++pattern) {
      /* Long-marker cases shorter than a run would duplicate literal cases. */
      if(lengths[index] <= 256 &&
         (pattern == PLUS_LONG || pattern == PERCENT_LONG))
        continue;
      for(form = 0; form < 2; ++form)
        if(run_case(lengths[index], (enum pattern)pattern, form, iterations)) {
          fputs("Benchmark validation or timer failed\n", stderr);
          return 1;
        }
    }
  printf("# checksum: %" PRIu64 "\n", checksum);
  return 0;
}
