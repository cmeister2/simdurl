/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long assertions;
static unsigned int case_flags;
static size_t case_length, case_capacity;
static const char *operation;

static void check(int condition, const char *expression, int line)
{
  ++assertions;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s (%s, length=%lu, capacity=%lu, flags=%u)\n",
            __FILE__, line, expression, operation, (unsigned long)case_length,
            (unsigned long)case_capacity, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

typedef simdurl_result (*codec)(const char *, size_t, char *, size_t, unsigned int);

static void test_arguments(codec fn, const char *name, unsigned int allowed)
{
  unsigned int flags, null_input, null_output;
  size_t length, capacity;
  char source = 'x', storage[3];
  operation = name;
  for(flags = 0; flags <= 7; ++flags) {
    for(null_input = 0; null_input <= 1; ++null_input) {
      for(null_output = 0; null_output <= 1; ++null_output) {
        for(length = 0; length <= 1; ++length) {
          for(capacity = 0; capacity <= 1; ++capacity) {
            simdurl_result actual;
            simdurl_status expected;
            case_flags = flags;
            case_length = length;
            case_capacity = capacity;
            memset(storage, 0xa5, sizeof(storage));
            if((flags & ~allowed) || (null_input && length) ||
               (null_output && capacity))
              expected = SIMDURL_INVALID_ARGUMENT;
            else if(!length)
              expected = SIMDURL_OK;
            else if(!capacity)
              expected = SIMDURL_BUFFER_TOO_SMALL;
            else
              expected = SIMDURL_OK;
            actual = fn(null_input ? NULL : &source, length,
                        null_output ? NULL : storage + 1, capacity, flags);
            CHECK(actual.status == expected);
            CHECK(actual.written == (expected == SIMDURL_OK ? length : 0));
            CHECK((unsigned char)storage[0] == 0xa5);
            CHECK((unsigned char)storage[2] == 0xa5);
            if(!capacity)
              CHECK((unsigned char)storage[1] == 0xa5);
            if(actual.status == SIMDURL_OK && actual.written)
              CHECK(storage[1] == source);
            CHECK(source == 'x');
          }
        }
      }
    }
  }
  /* Every unknown bit, alone and combined with each valid flag set. */
  for(flags = 1; flags; flags <<= 1) {
    unsigned int known;
    if(!(flags & ~allowed))
      continue;
    for(known = 0; known <= allowed; ++known) {
      simdurl_result actual;
      case_flags = flags | known;
      case_length = 0;
      case_capacity = 0;
      actual = fn(NULL, 0, NULL, 0, case_flags);
      CHECK(actual.status == SIMDURL_INVALID_ARGUMENT && actual.written == 0);
      case_length = 1;
      case_capacity = 1;
      actual = fn(&source, 1, storage + 1, 1, case_flags);
      CHECK(actual.status == SIMDURL_INVALID_ARGUMENT && actual.written == 0);
    }
  }
}

static void test_arithmetic(void)
{
  const size_t limit = SIZE_MAX / 3;
  const size_t lengths[] = { 0, 1, 2, limit - 1, limit, limit + 1, SIZE_MAX - 1, SIZE_MAX };
  size_t i;
  operation = "encode arithmetic";
  case_flags = 0;
  case_capacity = 1;
  for(i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    size_t length = lengths[i];
    case_length = length;
    CHECK(simdurl_encode_bound(length) == (length > limit ? SIZE_MAX : length * 3));
    if(length > limit) {
      char output = 'z';
      /* Such lengths must be rejected before accessing this one-byte source. */
      simdurl_result result = simdurl_encode("x", length, &output, 1, SIMDURL_URI);
      CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
    }
  }
}

static void test_scanner_arguments(void)
{
  unsigned int flags, unknown;
  operation = "scanner arguments";
  case_capacity = 0;
  for(flags = 0; flags < 8; ++flags) {
    case_flags = flags;
    case_length = 0;
    CHECK(simdurl_validate_bytes(NULL, 0, flags) == SIMDURL_OK);
    CHECK(simdurl_validate_bytes("x", 0, flags) == SIMDURL_OK);
    case_length = 1;
    CHECK(simdurl_validate_bytes(NULL, 1, flags) == SIMDURL_INVALID_ARGUMENT);
    CHECK(simdurl_validate_bytes("x", 1, flags) == SIMDURL_OK);
    case_length = SIZE_MAX;
    CHECK(simdurl_validate_bytes(NULL, SIZE_MAX, flags) == SIMDURL_INVALID_ARGUMENT);
    for(unknown = 8; unknown; unknown <<= 1) {
      case_flags = flags | unknown;
      case_length = 0;
      CHECK(simdurl_validate_bytes(NULL, 0, case_flags) == SIMDURL_INVALID_ARGUMENT);
      case_length = 1;
      CHECK(simdurl_validate_bytes("x", 1, case_flags) == SIMDURL_INVALID_ARGUMENT);
    }
  }
}

int main(void)
{
  test_arguments(simdurl_encode, "encode arguments", SIMDURL_FORM);
  test_arguments(simdurl_decode, "decode arguments",
                 SIMDURL_FORM | SIMDURL_REJECT_NUL | SIMDURL_REJECT_CONTROL);
  test_arithmetic();
  test_scanner_arguments();
  printf("contract: %lu assertions passed\n", assertions);
  return EXIT_SUCCESS;
}
