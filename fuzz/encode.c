/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#define FUZZ_CODEC 1
#define FUZZ_ENCODE 1
#include "common.h"
#include "reference.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  fuzz_case test = fuzz_parse(data, size, 1);
  char expected[FUZZ_MAX_INPUT * 3];
  size_t expected_size = reference_encode(test.input, test.length, expected,
                                          test.flags);
  size_t capacity = fuzz_capacity(&test, expected_size, test.length * 3);
  fuzz_buffer source = fuzz_allocate(test.length, test.source_offset,
                                     test.exact_allocation);
  fuzz_buffer destination = fuzz_allocate(capacity, test.output_offset,
                                          test.exact_allocation);
  const char *input = (test.control & 2) ? NULL : source.data;
  char *output = (test.control & 4) ? NULL : destination.data;
  int invalid = (test.flags & ~1u) || (!input && test.length) ||
                (!output && capacity);
  simdurl_result actual;
  memcpy(source.data, test.input, test.length);
  fuzz_protect(&source);
  fuzz_protect(&destination);
  FUZZ_CHECK(simdurl_encode_bound(test.length) == test.length * 3,
             &test, capacity);
  actual = simdurl_encode(input, test.length, output, capacity, test.flags);
  fuzz_verify_result(actual, &test, capacity, invalid, 0, output, expected,
                      expected_size);
  fuzz_unprotect(&source);
  fuzz_unprotect(&destination);
  FUZZ_CHECK(memcmp(source.data, test.input, test.length) == 0, &test, capacity);
  fuzz_verify_canaries(&source, &test, capacity);
  fuzz_verify_canaries(&destination, &test, capacity);
  free(source.allocation);
  free(destination.allocation);
  return 0;
}
