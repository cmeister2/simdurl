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
  char lowered[] = "Az\0Z";
  char hex[8];
  const simdurl_result lower = simdurl_ascii_lower(lowered, 4, lowered, 4);
  const simdurl_result encoded_hex = simdurl_hex_encode(lowered, 4, hex,
                                                       sizeof(hex), SIMDURL_HEX_LOWER);
  if(lower.status != SIMDURL_OK || lower.written != 4 ||
     std::memcmp(lowered, "az\0z", 4) != 0 ||
     encoded_hex.status != SIMDURL_OK || encoded_hex.written != sizeof(hex) ||
     std::memcmp(hex, "617a007a", sizeof(hex)) != 0 ||
     simdurl_hex_encode_bound(4) != sizeof(hex))
    return 1;
  return result.status != SIMDURL_OK || result.written != sizeof(expected) ||
         std::memcmp(decoded, expected, sizeof(expected)) != 0 ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_C0) !=
           SIMDURL_REJECTED ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_DEL) !=
           SIMDURL_OK;
}
