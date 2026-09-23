/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#ifndef SIMDURL_BENCH_SAMPLING_H
#define SIMDURL_BENCH_SAMPLING_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Changes to this policy require a new published measurement version. */
#define BENCH_SAMPLE_COUNT 20
#define BENCH_WARMUP_NS UINT64_C(100000000)
#define BENCH_TARGET_NS UINT64_C(100000000)
#define BENCH_MIN_SAMPLE_NS UINT64_C(50000000)
#define BENCH_MAX_ATTEMPTS 32
#define BENCH_MAX_ITERATIONS UINT64_C(1000000000)

/* The callback times a whole batch, keeping sampling outside the hot loop. */
typedef int (*bench_batch)(void *context, size_t iterations,
                           uint64_t *elapsed_ns);

static int bench_elapsed_ns(clock_t start, clock_t end, uint64_t *elapsed_ns)
{
  long double elapsed;
  if(start == (clock_t)-1 || end == (clock_t)-1 || end < start)
    return 1;
  elapsed = ((long double)end - (long double)start) * 1e9L /
            (long double)CLOCKS_PER_SEC;
  if(elapsed < 0 || elapsed >= (long double)UINT64_MAX)
    return 1;
  *elapsed_ns = (uint64_t)elapsed;
  return 0;
}

static void bench_sampling_header(void)
{
  puts("# simdurl calibrated v1");
  puts("benchmark,phase,sample,iterations,elapsed_ns");
}

static void bench_sample_row(const char *name, const char *phase,
                              unsigned int sample, size_t iterations,
                              uint64_t elapsed_ns)
{
  printf("%s,%s,%u,%" PRIuMAX ",%" PRIu64 "\n", name, phase, sample,
         (uintmax_t)iterations, elapsed_ns);
}

/* Limit growth to 16x so a near-zero initial timer reading cannot request an
 * enormous batch. Fail when calibration cannot make progress within bounds. */
static int bench_grow_iterations(size_t *iterations, uint64_t elapsed_ns)
{
  long double factor = elapsed_ns ?
    (long double)BENCH_TARGET_NS / (long double)elapsed_ns : 16.0L;
  long double next;
  if(factor < 1.1L)
    factor = 1.1L;
  if(factor > 16.0L)
    factor = 16.0L;
  next = (long double)*iterations * factor + 1.0L;
  if(next > (long double)SIZE_MAX || next > BENCH_MAX_ITERATIONS)
    return 1;
  *iterations = (size_t)next;
  return 0;
}

static int bench_sample_case(const char *name, size_t seed, bench_batch batch,
                              void *context)
{
  uint64_t elapsed_ns, warmed_ns = 0;
  size_t iterations = seed > 1000000 ? 1000000 : seed;
  unsigned int attempt, sample, retries = 0;
  if(!iterations)
    return 1;

  /* Every case gets the same minimum CPU warmup duration. These batches and
   * all subsequent calibration/retry batches are recorded but not published. */
  for(attempt = 0; attempt < BENCH_MAX_ATTEMPTS; ++attempt) {
    if(batch(context, iterations, &elapsed_ns))
      return 1;
    bench_sample_row(name, "warmup", 0, iterations, elapsed_ns);
    if(UINT64_MAX - warmed_ns < elapsed_ns)
      return 1;
    warmed_ns += elapsed_ns;
    if(warmed_ns >= BENCH_WARMUP_NS)
      break;
    if(bench_grow_iterations(&iterations, elapsed_ns))
      return 1;
  }
  if(warmed_ns < BENCH_WARMUP_NS)
    return 1;

  for(attempt = 0; attempt < BENCH_MAX_ATTEMPTS; ++attempt) {
    if(batch(context, iterations, &elapsed_ns))
      return 1;
    bench_sample_row(name, "calibration", 0, iterations, elapsed_ns);
    if(elapsed_ns >= BENCH_TARGET_NS)
      break;
    if(bench_grow_iterations(&iterations, elapsed_ns))
      return 1;
  }
  if(attempt == BENCH_MAX_ATTEMPTS)
    return 1;

  for(sample = 1; sample <= BENCH_SAMPLE_COUNT;) {
    if(batch(context, iterations, &elapsed_ns))
      return 1;
    if(elapsed_ns < BENCH_MIN_SAMPLE_NS) {
      bench_sample_row(name, "retry", 0, iterations, elapsed_ns);
      if(++retries > BENCH_MAX_ATTEMPTS ||
         bench_grow_iterations(&iterations, elapsed_ns))
        return 1;
      continue;
    }
    bench_sample_row(name, "sample", sample++, iterations, elapsed_ns);
  }
  return ferror(stdout) ? 1 : 0;
}

#endif
