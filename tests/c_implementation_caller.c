/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
  if(!(expression)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression); \
    return EXIT_FAILURE; \
  } \
} while(0)

int main(void)
{
  static const unsigned char group[] = { 'A', ' ', '+', '%', 0, 128, '~', '*' };
  static const char form_group[] = "A+%2B%25%00%80%7E*";
  static const char uri_group[] = "A%20%2B%25%00%80~%2A";
  char input[256], encoded[768], decoded[768], inplace[768];
  simdurl_result result;
  size_t i, encoded_length;
  unsigned int flags;
  for(i = 0; i < sizeof(input); ++i)
    input[i] = (char)group[i % sizeof(group)];
  CHECK(simdurl_encode_bound(sizeof(input)) == sizeof(encoded));
  for(flags = SIMDURL_URI; flags <= SIMDURL_FORM; ++flags) {
    const char *expected = flags == SIMDURL_FORM ? form_group : uri_group;
    size_t group_length = strlen(expected);
    result = simdurl_encode(input, sizeof(input), encoded, sizeof(encoded),
                            flags);
    CHECK(result.status == SIMDURL_OK);
    encoded_length = result.written;
    CHECK(encoded_length == sizeof(input) / sizeof(group) * group_length);
    for(i = 0; i < encoded_length; i += group_length)
      CHECK(memcmp(encoded + i, expected, group_length) == 0);
    result = simdurl_decode(encoded, encoded_length, decoded, sizeof(decoded),
                            flags);
    CHECK(result.status == SIMDURL_OK && result.written == sizeof(input));
    CHECK(memcmp(decoded, input, sizeof(input)) == 0);
    memcpy(inplace, encoded, encoded_length);
    result = simdurl_decode(inplace, encoded_length, inplace, sizeof(inplace),
                            flags);
    CHECK(result.status == SIMDURL_OK && result.written == sizeof(input));
    CHECK(memcmp(inplace, input, sizeof(input)) == 0);
    result = simdurl_decode(encoded, encoded_length, decoded, sizeof(decoded),
                            flags | SIMDURL_REJECT_NUL);
    CHECK(result.status == SIMDURL_REJECTED && result.written == 0);
    CHECK(simdurl_validate_bytes(encoded, encoded_length, SIMDURL_CHECK_C0 |
      SIMDURL_CHECK_DEL | SIMDURL_CHECK_SPACE) == SIMDURL_OK);
  }
  CHECK(simdurl_validate_bytes(input, sizeof(input), SIMDURL_CHECK_C0) ==
        SIMDURL_REJECTED);
  CHECK(simdurl_validate_bytes(input, sizeof(input), SIMDURL_CHECK_DEL) ==
        SIMDURL_OK);
  CHECK(simdurl_validate_bytes(NULL, 0, SIMDURL_CHECK_C0) == SIMDURL_OK);
  CHECK(simdurl_validate_bytes(NULL, 1, SIMDURL_CHECK_C0) ==
        SIMDURL_INVALID_ARGUMENT);
  puts("C caller with C++ implementation passed");
  return EXIT_SUCCESS;
}
