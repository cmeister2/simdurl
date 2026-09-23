/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <cstring>

int simdurl_cpp_other()
{
  char decoded[] = "%41%00+%2B";
  const char expected[] = { 'A', '\0', ' ', '+' };
  const simdurl_result result = simdurl_decode(decoded, sizeof(decoded) - 1,
                                              decoded, sizeof(decoded), SIMDURL_FORM);
  return result.status != SIMDURL_OK || result.written != sizeof(expected) ||
         std::memcmp(decoded, expected, sizeof(expected)) != 0 ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_C0) !=
           SIMDURL_REJECTED ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_DEL) !=
           SIMDURL_OK;
}
