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
#define BENCH_BARRIER(pointer) do { (void)(pointer); _ReadWriteBarrier(); } while(0)
#elif defined(__GNUC__) || defined(__clang__)
#define BENCH_BARRIER(pointer) __asm__ __volatile__("" : : "r"(pointer) : "memory")
#else
#define BENCH_BARRIER(pointer) ((void)(pointer))
#endif

static volatile uint64_t checksum;

typedef simdurl_result (*codec_function)(const char *, size_t, char *, size_t,
                                        unsigned int);

static void fill_input(char *input, size_t length, unsigned int pattern,
                       int decode)
{
  const char *text;
  size_t i, text_length;
  if(!decode && pattern == 2) {
    for(i = 0; i < length; ++i)
      input[i] = (char)((i & 1) ? (128 + i % 128) : (i % 32));
    return;
  }
  if(pattern == 0)
    text = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._";
  else if(decode && pattern == 1)
    text = "alpha%20beta%2B+gamma%2Fdelta";
  else if(decode)
    text = "%00%FF%2B%20%61%2f";
  else
    text = "hello world/with+symbols?x=3&y=*~";
  text_length = strlen(text);
  for(i = 0; i < length; ++i)
    input[i] = text[i % text_length];
}

static int run_case(size_t length, unsigned int pattern, int decode,
                    unsigned int flags, size_t iterations)
{
  static const char *const patterns[] = { "literal", "mixed", "dense" };
  char input[4096], output[4096 * 3];
  codec_function operation = decode ? simdurl_decode : simdurl_encode;
  simdurl_result initial, result;
  clock_t start, end;
  double elapsed;
  size_t iteration, capacity = decode ? length : simdurl_encode_bound(length);
  fill_input(input, length, pattern, decode);
  initial = operation(input, length, output, capacity, flags);
  if(initial.status != SIMDURL_OK)
    return 1;
  /* Warm the instruction and data caches before timing. */
  for(iteration = 0; iteration < 100; ++iteration) {
    result = operation(input, length, output, capacity, flags);
    checksum += (unsigned char)output[iteration % result.written];
    BENCH_BARRIER(output);
  }
  start = clock();
  for(iteration = 0; iteration < iterations; ++iteration) {
    result = operation(input, length, output, capacity, flags);
    if(result.status != SIMDURL_OK || result.written != initial.written)
      return 1;
    checksum += (unsigned char)output[iteration % result.written];
    /* Keep each write visible to the compiler, including under LTO. */
    BENCH_BARRIER(output);
  }
  end = clock();
  if(start == (clock_t)-1 || end == (clock_t)-1)
    return 1;
  elapsed = (double)(end - start) / (double)CLOCKS_PER_SEC;
  printf("%-6s %-4s %-7s %5lu  %10.3f  %9.3f\n",
         decode ? "decode" : "encode", flags ? "form" : "URI", patterns[pattern],
         (unsigned long)length, elapsed * 1000.0,
         elapsed > 0 ? ((double)length * (double)iterations) / elapsed / 1e9 : 0.0);
  return 0;
}

int main(int argc, char **argv)
{
  static const size_t lengths[] = { 16, 128, 4096 };
  size_t iterations = 10000, length_index;
  unsigned int pattern, flags;
  int decode, core = argc == 3;
  if(argc > 3 || (core && strcmp(argv[2], "--core"))) {
    fprintf(stderr, "Usage: %s [iterations_per_case] [--core]\n", argv[0]);
    return 1;
  }
  if(argc >= 2) {
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(argv[1], &end, 10);
    if(errno || end == argv[1] || *end || argv[1][0] == '-' ||
       !parsed || parsed > SIZE_MAX) {
      fprintf(stderr, "iterations_per_case must be a positive integer\n");
      return 1;
    }
    iterations = (size_t)parsed;
  }
#ifdef SIMDURL_DISABLE_SIMD
  puts("Backend: portable scalar (header-only)");
#else
  puts("Backend: automatic CPU selection (header-only)");
#endif
  printf("Iterations per case: %lu; throughput counts input bytes; CPU time.\n",
         (unsigned long)iterations);
  puts("codec  mode pattern bytes   elapsed_ms       GB/s");
  for(length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]);
      ++length_index)
    for(pattern = 0; pattern < 3; ++pattern)
      for(flags = 0; flags < 2; ++flags)
        for(decode = 0; decode < 2; ++decode) {
          if(core && !(
             (lengths[length_index] == 128 && pattern == 1 &&
              ((!decode && flags == SIMDURL_URI) ||
               (decode && flags == SIMDURL_FORM))) ||
             (lengths[length_index] == 4096 && flags == SIMDURL_URI &&
              ((!decode && pattern == 0) || pattern == 2))))
            continue;
          if(run_case(lengths[length_index], pattern, decode, flags, iterations)) {
            fputs("Benchmark operation failed\n", stderr);
            return 1;
          }
        }
  printf("Checksum: %" PRIu64 "\n", checksum);
  return 0;
}
