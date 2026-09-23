/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>
#include <simdurl.h>

#include <cstring>

int simdurl_cpp_other();

int main()
{
  char encoded[64];
  const simdurl_result result = simdurl_encode("a b+~", 5, encoded,
                                              sizeof(encoded), SIMDURL_FORM);
  if(result.status != SIMDURL_OK || result.written != 9 ||
     std::memcmp(encoded, "a+b%2B%7E", 9) != 0)
    return 1;
  const unsigned int checks = SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL |
                              SIMDURL_CHECK_SPACE;
  if(simdurl_validate_bytes("a b", 3, checks) != SIMDURL_REJECTED ||
     simdurl_validate_bytes(encoded, result.written, checks) != SIMDURL_OK)
    return 1;
  return simdurl_cpp_other();
}
