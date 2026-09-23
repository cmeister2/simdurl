/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

enum { INPUT_COUNT = 16, MAX_LENGTH = 4096, REPEATS = 5 };
static volatile uint64_t checksum;
typedef simdurl_status (*scan_function)(const char *, size_t);

/* Independent branch-per-byte comparator. Ordinary compiler optimizations,
 * including automatic vectorization, remain enabled for this portable C. */
static inline simdurl_status branch_scan(const char *input, size_t length,
                                         unsigned int checks)
{
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char byte = (unsigned char)input[i];
    if(((checks & SIMDURL_CHECK_C0) && byte < 0x20) ||
       ((checks & SIMDURL_CHECK_DEL) && byte == 0x7f) ||
       ((checks & SIMDURL_CHECK_SPACE) && byte == 0x20))
      return SIMDURL_REJECTED;
  }
  return SIMDURL_OK;
}

/* Fixed policies allow the header-only implementation and the comparator to
 * specialize equally. Both pay one non-inlined function call per operation. */
static BENCH_NOINLINE simdurl_status branch_controls(const char *s, size_t n)
{
  return branch_scan(s, n, SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL);
}

static BENCH_NOINLINE simdurl_status library_controls(const char *s, size_t n)
{
  return simdurl_validate_bytes(s, n, SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL);
}

static BENCH_NOINLINE simdurl_status branch_controls_space(const char *s,
                                                          size_t n)
{
  return branch_scan(s, n, SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL |
                           SIMDURL_CHECK_SPACE);
}

static BENCH_NOINLINE simdurl_status library_controls_space(const char *s,
                                                           size_t n)
{
  return simdurl_validate_bytes(s, n, SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL |
                                      SIMDURL_CHECK_SPACE);
}

static void fill_inputs(char input[INPUT_COUNT][MAX_LENGTH + INPUT_COUNT],
                         size_t length, unsigned int pattern,
                         unsigned int spaces)
{
  size_t row, i;
  for(row = 0; row < INPUT_COUNT; ++row) {
    char *data = input[row] + row;
    for(i = 0; i < length; ++i)
      data[i] = pattern == 3 ? (char)(128 + (i + row) % 128) :
                             (char)('!' + (i + row) % 94);
    if(length && (pattern == 1 || pattern == 2)) {
      static const unsigned char bad[] = { 0x1f, 0x7f, 0x20 };
      data[pattern == 1 ? 0 : length - 1] = (char)bad[row % (spaces ? 3 : 2)];
    }
  }
}

static double measure(scan_function scan,
                      char input[INPUT_COUNT][MAX_LENGTH + INPUT_COUNT],
                      size_t length, size_t iterations)
{
  uint64_t sum = 0;
  size_t i;
  clock_t start, end;
  start = clock();
  for(i = 0; i < iterations; ++i) {
    size_t row = i % INPUT_COUNT;
    const char *data = input[row] + row;
    /* The compiler must reload the input on every operation. */
    BENCH_BARRIER(data);
    sum += (unsigned int)scan(data, length) + 1;
  }
  end = clock();
  checksum += sum;
  if(start == (clock_t)-1 || end == (clock_t)-1 || end < start)
    return -1.0;
  return (double)(end - start) * 1e9 / (double)CLOCKS_PER_SEC /
         (double)iterations;
}

static int run_case(size_t length, unsigned int pattern, unsigned int spaces,
                    size_t iterations)
{
  static const char *const patterns[] = {
    "valid", "first_forbidden", "last_forbidden", "high_bytes"
  };
  static const char *const variants[] = { "portable_C_branch", "simdurl" };
  char input[INPUT_COUNT][MAX_LENGTH + INPUT_COUNT];
  scan_function scans[2];
  simdurl_status expected = length && (pattern == 1 || pattern == 2) ?
                             SIMDURL_REJECTED : SIMDURL_OK;
  unsigned int repeat, order, variant;
  size_t row;
  scans[0] = spaces ? branch_controls_space : branch_controls;
  scans[1] = spaces ? library_controls_space : library_controls;
  fill_inputs(input, length, pattern, spaces);
  for(variant = 0; variant < 2; ++variant) {
    for(row = 0; row < INPUT_COUNT; ++row)
      if(scans[variant](input[row] + row, length) != expected)
        return 1;
    if(measure(scans[variant], input, length, 1000) < 0)
      return 1;
  }
  for(repeat = 0; repeat < REPEATS; ++repeat) {
    /* Alternate order to reduce consistent first/second-run bias. */
    for(order = 0; order < 2; ++order) {
      double ns;
      variant = (order + repeat) % 2;
      ns = measure(scans[variant], input, length, iterations);
      if(ns < 0)
        return 1;
      printf("%s,%s,%lu,%s,%u,%.3f\n",
             spaces ? "C0_DEL_SPACE" : "C0_DEL", patterns[pattern],
             (unsigned long)length, variants[variant], repeat + 1, ns);
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  static const size_t lengths[] = { 0, 8, 16, 31, 32, 64, 128, 512, 4096 };
  size_t iterations = 100000, length_index;
  unsigned int pattern, spaces;
  if(argc > 2) {
    fprintf(stderr, "Usage: %s [iterations_per_sample]\n", argv[0]);
    return 1;
  }
  if(argc == 2) {
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
#ifdef SIMDURL_DISABLE_SIMD
  puts("# simdurl: portable C; compiler-generated SIMD remains permitted");
#else
  puts("# simdurl: automatic CPU selection (header-only)");
#endif
  puts("# comparator: independent portable C branch loop; compiler optimization enabled");
  printf("# %lu iterations/sample; %u samples/case; CPU time; nanoseconds/operation\n",
         (unsigned long)iterations, (unsigned int)REPEATS);
  puts("# Early rejection may inspect only a prefix; no full-buffer throughput is reported");
  puts("checks,pattern,bytes,variant,sample,ns_per_op");
  for(length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]);
      ++length_index)
    for(spaces = 0; spaces < 2; ++spaces)
      for(pattern = 0; pattern < 4; ++pattern) {
        if(!lengths[length_index] && pattern != 0)
          continue;
        if(run_case(lengths[length_index], pattern, spaces, iterations)) {
          fputs("Benchmark validation or timer failed\n", stderr);
          return 1;
        }
      }
  printf("# checksum: %" PRIu64 "\n", checksum);
  return 0;
}
