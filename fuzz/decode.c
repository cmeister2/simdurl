/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#define FUZZ_CODEC 1
#define FUZZ_DECODE 1
#include "common.h"
#include "reference.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  fuzz_case test = fuzz_parse(data, size, 7);
  char expected[FUZZ_MAX_INPUT];
  int rejected, inplace;
  size_t expected_size = reference_decode(test.input, test.length, expected,
                                          test.flags, &rejected);
  size_t capacity = fuzz_capacity(&test, expected_size, test.length);
  for(inplace = 0; inplace <= 1; ++inplace) {
    size_t span = inplace && capacity > test.length ? capacity : test.length;
    size_t i;
    fuzz_buffer source = fuzz_allocate(span, test.source_offset,
                                       test.exact_allocation);
    fuzz_buffer destination = fuzz_allocate(capacity, test.output_offset,
                                            test.exact_allocation);
    const char *input = (test.control & 2) ? NULL : source.data;
    char *output = (test.control & 4) ? NULL :
                   inplace ? source.data : destination.data;
    int invalid = (test.flags & ~7u) || (!input && test.length) ||
                  (!output && capacity);
    simdurl_result actual;
    memcpy(source.data, test.input, test.length);
    fuzz_protect(&source);
    fuzz_protect(&destination);
    actual = simdurl_decode(input, test.length, output, capacity, test.flags);
    fuzz_verify_result(actual, &test, capacity, invalid, rejected, output,
                        expected, expected_size);
    fuzz_unprotect(&source);
    fuzz_unprotect(&destination);
    if(!inplace)
      FUZZ_CHECK(memcmp(source.data, test.input, test.length) == 0,
                 &test, capacity);
    else {
      /* These bytes are readable input but lie outside advertised output
       * capacity, so ASan cannot poison them during an in-place call. */
      for(i = capacity; i < test.length; ++i)
        FUZZ_CHECK((uint8_t)source.data[i] == test.input[i], &test, capacity);
    }
    fuzz_verify_canaries(&source, &test, capacity);
    fuzz_verify_canaries(&destination, &test, capacity);
    free(source.allocation);
    free(destination.allocation);
  }
  return 0;
}
