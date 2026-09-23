/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>
#include <string.h>

int main(void)
{
  static const char input[] = { 'a', ' ', '+', '\0', (char)0xff };
  char encoded[sizeof(input) * 3], decoded[sizeof(input)];
  simdurl_result result = simdurl_encode(input, sizeof(input), encoded,
                                        sizeof(encoded), SIMDURL_FORM);
  const size_t encoded_length = result.written;
  if(result.status != SIMDURL_OK || result.written != 11 ||
     memcmp(encoded, "a+%2B%00%FF", 11) != 0)
    return 1;
  result = simdurl_decode(encoded, encoded_length, decoded, sizeof(decoded), SIMDURL_FORM);
  if(result.status != SIMDURL_OK || result.written != sizeof(input) ||
     memcmp(decoded, input, sizeof(input)) != 0)
    return 1;
  {
    char lowered[] = { 'A', 0, 'Z', (char)255 };
    const char expected[] = { 'a', 0, 'z', (char)255 };
    char hex[sizeof(lowered) * 2];
    simdurl_result lower = simdurl_ascii_lower(lowered, sizeof(lowered), lowered,
                                               sizeof(lowered));
    simdurl_result hex_result = simdurl_hex_encode(lowered, sizeof(lowered), hex,
                                                  sizeof(hex), SIMDURL_HEX_LOWER);
    if(lower.status != SIMDURL_OK || lower.written != sizeof(lowered) ||
       memcmp(lowered, expected, sizeof(expected)) != 0 ||
       hex_result.status != SIMDURL_OK || hex_result.written != sizeof(hex) ||
       memcmp(hex, "61007aff", sizeof(hex)) != 0 ||
       simdurl_hex_encode_bound(sizeof(lowered)) != sizeof(hex))
      return 1;
    hex_result = simdurl_hex_encode(lowered, sizeof(lowered), hex, sizeof(hex),
                                     SIMDURL_HEX_UPPER);
    if(hex_result.status != SIMDURL_OK || hex_result.written != sizeof(hex) ||
       memcmp(hex, "61007AFF", sizeof(hex)) != 0)
      return 1;
  }
  return result.status != SIMDURL_OK || result.written != sizeof(input) ||
         memcmp(decoded, input, sizeof(input)) != 0 ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_C0) !=
           SIMDURL_REJECTED ||
         simdurl_validate_bytes(decoded, result.written, SIMDURL_CHECK_DEL) !=
           SIMDURL_OK ||
         simdurl_validate_bytes(encoded, encoded_length,
                                SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL |
                                SIMDURL_CHECK_SPACE) != SIMDURL_OK;
}
