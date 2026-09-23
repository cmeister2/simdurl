/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These implementation-specific checks exercise every available encoder even
 * when public CPU dispatch prefers a newer backend. Internal names are not API.
 */
#ifdef SIMDURL_DETAIL_X86
typedef void (*encode_backend)(const char **, size_t *, char **, unsigned int);

static unsigned long checks;
static const char *backend_name;
static size_t case_length;
static unsigned int case_flags;

static void check(int condition, const char *expression, int line)
{
  ++checks;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed (%s, length=%lu, flags=%u)\n",
            __FILE__, line, expression, backend_name,
            (unsigned long)case_length, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

static size_t reference_encode(const char *input, size_t length, char *output,
                               unsigned int flags)
{
  static const char alphabet[] = "0123456789ABCDEF";
  size_t i, written = 0;
  for(i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)input[i];
    int unchanged = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' ||
                    c == ((flags & SIMDURL_FORM) ? '*' : '~');
    if(unchanged)
      output[written++] = (char)c;
    else if(c == ' ' && (flags & SIMDURL_FORM))
      output[written++] = '+';
    else {
      output[written++] = '%';
      output[written++] = alphabet[c >> 4];
      output[written++] = alphabet[c & 15];
    }
  }
  return written;
}

static void check_case(encode_backend backend, const char *input, size_t length,
                       unsigned int flags, size_t offset)
{
  size_t capacity = length * 3, remaining = length, consumed, expected, i;
  char *source_storage = (char *)malloc(length + offset + 1);
  char *storage = (char *)malloc(capacity + offset + 32);
  char *reference = (char *)malloc(capacity + 1);
  char *output;
  const char *next;
  case_length = length;
  case_flags = flags;
  CHECK(source_storage != NULL && storage != NULL && reference != NULL);
  memcpy(source_storage + offset, input, length);
  memset(storage, 0x5a, capacity + offset + 32);
  output = storage + offset;
  next = source_storage + offset;

  backend(&next, &remaining, &output, flags);

  CHECK(remaining <= length);
  consumed = length - remaining;
  CHECK(next == source_storage + offset + consumed);
  CHECK(remaining < 32);
  expected = reference_encode(input, consumed, reference, flags);
  CHECK(output == storage + offset + expected);
  CHECK(memcmp(storage + offset, reference, expected) == 0);
  CHECK(memcmp(source_storage + offset, input, length) == 0);
  for(i = 0; i < offset; ++i)
    CHECK((unsigned char)storage[i] == 0x5a);
  for(i = offset + capacity; i < offset + capacity + 32; ++i)
    CHECK((unsigned char)storage[i] == 0x5a);

  free(reference);
  free(storage);
  free(source_storage);
}

static uint32_t random_word(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void test_backend(encode_backend backend, const char *name)
{
  static const char literal[] = "aZ09-._";
  static const char mixed[] = "aZ09-._~* +%/\x80\xff";
  char input[2048];
  uint32_t seed = UINT32_C(0x35dabc97);
  unsigned int flags, byte, position, trial;
  size_t length, i;
  backend_name = name;

  for(flags = SIMDURL_URI; flags <= SIMDURL_FORM; ++flags) {
    for(byte = 0; byte < 256; ++byte) {
      memset(input, (int)byte, sizeof(input));
      for(length = 0; length <= 128; ++length)
        check_case(backend, input, length, flags, length % 32);
    }
    for(byte = 0; byte < 256; ++byte) {
      for(position = 0; position < 32; ++position) {
        memset(input, 'a', sizeof(input));
        input[position] = (char)byte;
        check_case(backend, input, 64, flags, position);
      }
    }
    for(length = 0; length <= sizeof(input); length += 17) {
      for(i = 0; i < length; ++i)
        input[i] = literal[i % (sizeof(literal) - 1)];
      check_case(backend, input, length, flags, length % 32);
      for(i = 0; i < length; ++i)
        input[i] = mixed[i % (sizeof(mixed) - 1)];
      check_case(backend, input, length, flags, length % 32);
    }
    for(trial = 0; trial < 2000; ++trial) {
      length = random_word(&seed) % (sizeof(input) + 1);
      for(i = 0; i < length; ++i)
        input[i] = (char)random_word(&seed);
      check_case(backend, input, length, flags, trial % 32);
    }
  }
  printf("%s encoder passed\n", name);
}
#endif

int main(void)
{
#ifdef SIMDURL_DETAIL_X86
  if(simdurl_detail_has_avx2())
    test_backend(simdurl_detail_encode_avx2, "AVX2");
  if(simdurl_detail_has_vbmi2())
    test_backend(simdurl_detail_encode_vbmi2, "AVX-512 VBMI2");
  printf("%lu direct backend checks passed\n", checks);
#else
  puts("SIMD backends unavailable in this build; portable tests cover fallback");
#endif
  return EXIT_SUCCESS;
}
