/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#ifndef SIMDURL_FUZZ_COMMON_H
#define SIMDURL_FUZZ_COMMON_H

#include <simdurl.h>
#include <sanitizer/asan_interface.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_INPUT 4096
#define FUZZ_HEADER_SIZE 8
#define FUZZ_CANARY 0xa5

typedef struct fuzz_case {
  const uint8_t *input;
  size_t length;
  unsigned int flags, source_offset, output_offset, capacity_kind, control;
  size_t capacity_value;
  int exact_allocation;
} fuzz_case;

typedef struct fuzz_buffer {
  unsigned char *allocation;
  char *data;
  size_t prefix, span, total;
} fuzz_buffer;

static fuzz_case fuzz_parse(const uint8_t *data, size_t size, unsigned int mask)
{
  uint8_t header[FUZZ_HEADER_SIZE] = {0};
  fuzz_case test;
  size_t header_size = size < FUZZ_HEADER_SIZE ? size : FUZZ_HEADER_SIZE;
  memcpy(header, data, header_size);
  test.input = data + header_size;
  test.length = size - header_size;
  if(test.length > FUZZ_MAX_INPUT)
    test.length = FUZZ_MAX_INPUT;
  test.flags = header[0] & mask;
  test.source_offset = header[1] & 63;
  test.output_offset = header[2] & 63;
  test.capacity_kind = header[3] & 7;
  test.capacity_value = (size_t)header[4] | ((size_t)header[5] << 8);
  test.control = header[6];
  test.exact_allocation = header[7] & 1;
  if(test.control & 1)
    test.flags |= 0x100;
  return test;
}

static void fuzz_check(int condition, const char *expression,
                       const fuzz_case *test, size_t capacity, int line)
{
  if(!condition) {
    fprintf(stderr, "fuzz check failed at line %d: %s "
            "(length=%lu, flags=%u, capacity=%lu, source_offset=%u, "
            "output_offset=%u, control=%u, exact=%d)\n", line, expression,
            (unsigned long)test->length, test->flags, (unsigned long)capacity,
            test->source_offset, test->output_offset, test->control,
            test->exact_allocation);
    abort();
  }
}
#define FUZZ_CHECK(expr, test, capacity) \
  fuzz_check((expr), #expr, (test), (capacity), __LINE__)

static fuzz_buffer fuzz_allocate(size_t span, size_t offset, int exact)
{
  fuzz_buffer buffer;
  buffer.prefix = offset;
  buffer.span = span;
  buffer.total = offset + span + (exact ? 0 : 32);
  if(!buffer.total)
    buffer.total = 1;
  buffer.allocation = (unsigned char *)malloc(buffer.total);
  if(!buffer.allocation)
    abort();
  memset(buffer.allocation, FUZZ_CANARY, buffer.total);
  buffer.data = (char *)buffer.allocation + offset;
  return buffer;
}

/* Poison bytes outside a logical span, including alignment padding. Exact
 * allocation mode additionally puts the span end against malloc's redzone.
 * ASan can leave part of a leading shadow granule addressable; output canaries
 * still detect writes there. Deterministic guard-page tests cover both edges.
 */
static void fuzz_protect(fuzz_buffer *buffer)
{
  __asan_poison_memory_region(buffer->allocation, buffer->prefix);
  __asan_poison_memory_region(buffer->data + buffer->span,
    buffer->total - buffer->prefix - buffer->span);
}

static void fuzz_unprotect(fuzz_buffer *buffer)
{
  __asan_unpoison_memory_region(buffer->allocation, buffer->total);
}

static void fuzz_verify_canaries(const fuzz_buffer *buffer,
                                 const fuzz_case *test, size_t capacity)
{
  size_t i;
  for(i = 0; i < buffer->prefix; ++i)
    FUZZ_CHECK(buffer->allocation[i] == FUZZ_CANARY, test, capacity);
  for(i = buffer->prefix + buffer->span; i < buffer->total; ++i)
    FUZZ_CHECK(buffer->allocation[i] == FUZZ_CANARY, test, capacity);
}

#if defined(FUZZ_CODEC)
static size_t fuzz_capacity(const fuzz_case *test, size_t expected, size_t bound)
{
  switch(test->capacity_kind) {
    case 0: return bound;
    case 1: return expected;
    case 2: return expected ? expected - 1 : 0;
    case 3: return 0;
    case 4: return bound + 1;
    case 5: return test->capacity_value % (bound + 2);
    case 6: return test->length;
    default: return expected + 1;
  }
}

/* No capacity/rejection precedence is promised by the API. If both conditions
 * apply, either documented error is valid; successful results are always exact.
 */
static void fuzz_verify_result(simdurl_result actual, const fuzz_case *test,
                               size_t capacity, int invalid, int rejected,
                               const char *output, const char *expected,
                               size_t expected_size)
{
  if(invalid)
    FUZZ_CHECK(actual.status == SIMDURL_INVALID_ARGUMENT, test, capacity);
  else if(rejected && capacity < expected_size)
    FUZZ_CHECK(actual.status == SIMDURL_REJECTED ||
               actual.status == SIMDURL_BUFFER_TOO_SMALL, test, capacity);
  else if(rejected)
    FUZZ_CHECK(actual.status == SIMDURL_REJECTED, test, capacity);
  else if(capacity < expected_size)
    FUZZ_CHECK(actual.status == SIMDURL_BUFFER_TOO_SMALL, test, capacity);
  else {
    FUZZ_CHECK(actual.status == SIMDURL_OK, test, capacity);
    FUZZ_CHECK(actual.written == expected_size, test, capacity);
    if(expected_size)
      FUZZ_CHECK(memcmp(output, expected, expected_size) == 0, test, capacity);
  }
  if(actual.status != SIMDURL_OK)
    FUZZ_CHECK(actual.written == 0, test, capacity);
}
#endif
#endif
