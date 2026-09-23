/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include "common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  fuzz_case test = fuzz_parse(data, size, 7);
  fuzz_buffer source = fuzz_allocate(test.length, test.source_offset,
                                     test.exact_allocation);
  const char *input = (test.control & 2) ? NULL : source.data;
  simdurl_status expected = SIMDURL_OK;
  size_t i;
  memcpy(source.data, test.input, test.length);
  if((test.flags & ~7u) || (!input && test.length))
    expected = SIMDURL_INVALID_ARGUMENT;
  else {
    for(i = 0; i < test.length; ++i) {
      uint8_t byte = test.input[i];
      if(((test.flags & 1) && byte <= 31) ||
         ((test.flags & 2) && byte == 127) ||
         ((test.flags & 4) && byte == 32))
        expected = SIMDURL_REJECTED;
    }
  }
  fuzz_protect(&source);
  FUZZ_CHECK(simdurl_validate_bytes(input, test.length, test.flags) == expected,
             &test, 0);
  fuzz_unprotect(&source);
  FUZZ_CHECK(memcmp(source.data, test.input, test.length) == 0, &test, 0);
  fuzz_verify_canaries(&source, &test, 0);
  free(source.allocation);
  return 0;
}
