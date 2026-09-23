/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *capture;

static int capture_line(const char *line)
{
  return fputs(line, capture) == EOF ? EOF : fputc('\n', capture);
}

/* Capture the actual sampler records without running timed workloads or
 * depending on platform-specific file descriptor redirection. */
#define printf(...) fprintf(capture, __VA_ARGS__)
#define puts capture_line
#include "../benchmarks/bench_sampling.h"
#undef puts
#undef printf

static void check(int condition, const char *expression, int line)
{
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, line, expression);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

struct fake_clock {
  unsigned int calls, short_call, fail_call;
  int zero;
};

static int fake_batch(void *opaque, size_t iterations, uint64_t *elapsed_ns)
{
  struct fake_clock *context = opaque;
  CHECK(iterations > 0 && iterations <= BENCH_MAX_ITERATIONS);
  ++context->calls;
  if(context->calls == context->fail_call)
    return 1;
  *elapsed_ns = context->zero ? 0 :
    context->calls == context->short_call ? UINT64_C(49000000) :
    (uint64_t)iterations * 1000;
  return 0;
}

static void reset_capture(void)
{
  if(capture)
    CHECK(fclose(capture) == 0);
  capture = tmpfile();
  CHECK(capture != NULL);
}

static void test_sampling_and_short_retry(void)
{
  struct fake_clock context = { 0, 7, 0, 0 };
  uint64_t warmup = 0;
  unsigned int accepted = 0, retries = 0, rows = 0;
  int calibrated = 0;
  char line[256];
  reset_capture();
  bench_sampling_header();
  CHECK(bench_sample_case("fixture", 1000, fake_batch, &context) == 0);
  CHECK(fseek(capture, 0, SEEK_SET) == 0);
  CHECK(fgets(line, sizeof(line), capture) != NULL);
  CHECK(!strcmp(line, "# simdurl calibrated v1\n"));
  CHECK(fgets(line, sizeof(line), capture) != NULL);
  CHECK(!strcmp(line, "benchmark,phase,sample,iterations,elapsed_ns\n"));
  while(fgets(line, sizeof(line), capture)) {
    char name[80], phase[20], end;
    unsigned int sample;
    uintmax_t iterations;
    uint64_t elapsed_ns;
    CHECK(sscanf(line, "%79[^,],%19[^,],%u,%" SCNuMAX ",%" SCNu64 "%c",
                 name, phase, &sample, &iterations, &elapsed_ns, &end) == 6);
    CHECK(!strcmp(name, "fixture") && end == '\n');
    CHECK(iterations > 0 && iterations <= BENCH_MAX_ITERATIONS);
    ++rows;
    if(!strcmp(phase, "warmup")) {
      CHECK(!calibrated && !accepted && sample == 0);
      warmup += elapsed_ns;
    }
    else if(!strcmp(phase, "calibration")) {
      CHECK(warmup >= BENCH_WARMUP_NS && !accepted && sample == 0);
      calibrated = elapsed_ns >= BENCH_TARGET_NS;
    }
    else if(!strcmp(phase, "retry")) {
      CHECK(calibrated && sample == 0 && elapsed_ns < BENCH_MIN_SAMPLE_NS);
      ++retries;
    }
    else {
      CHECK(!strcmp(phase, "sample") && calibrated);
      CHECK(sample == ++accepted && elapsed_ns >= BENCH_MIN_SAMPLE_NS);
    }
  }
  CHECK(!ferror(capture));
  CHECK(accepted == BENCH_SAMPLE_COUNT && retries == 1);
  CHECK(rows == context.calls);
}

static void test_failures_are_bounded(void)
{
  struct fake_clock zero = { 0, 0, 0, 1 }, failure = { 0, 0, 2, 0 };
  size_t iterations = (size_t)BENCH_MAX_ITERATIONS;
  uint64_t elapsed_ns;
  reset_capture();
  CHECK(bench_sample_case("zero", 1, fake_batch, &zero) != 0);
  CHECK(zero.calls > 0 && zero.calls <= BENCH_MAX_ATTEMPTS);
  CHECK(bench_sample_case("failure", 1000, fake_batch, &failure) != 0);
  CHECK(failure.calls == 2);
  CHECK(bench_sample_case("empty", 0, fake_batch, &failure) != 0);
  CHECK(failure.calls == 2);
  CHECK(bench_grow_iterations(&iterations, 1) != 0);
  CHECK(iterations == BENCH_MAX_ITERATIONS);
  CHECK(bench_elapsed_ns((clock_t)0, (clock_t)CLOCKS_PER_SEC, &elapsed_ns) == 0);
  CHECK(elapsed_ns == UINT64_C(1000000000));
  CHECK(bench_elapsed_ns((clock_t)-1, (clock_t)0, &elapsed_ns) != 0);
  CHECK(bench_elapsed_ns((clock_t)0, (clock_t)-1, &elapsed_ns) != 0);
  CHECK(bench_elapsed_ns((clock_t)2, (clock_t)1, &elapsed_ns) != 0);
}

int main(void)
{
  test_sampling_and_short_retry();
  test_failures_are_bounded();
  CHECK(fclose(capture) == 0);
  return 0;
}
