/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#if defined(SIMDURL_TEST_POSIX)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <simdurl.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SIMDURL_TEST_POSIX)
#include <sys/mman.h>
#include <unistd.h>
#if defined(MAP_ANONYMOUS)
#define SIMDURL_TEST_MAP_ANON MAP_ANONYMOUS
#else
#define SIMDURL_TEST_MAP_ANON MAP_ANON
#endif
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef simdurl_result (*lower_function)(const char *, size_t, char *, size_t);
typedef simdurl_result (*hex_function)(const char *, size_t, char *, size_t,
                                     unsigned int);

static unsigned long assertions;
static const char *context = "initialization";
static const char *backend_name = "public API";
static size_t case_length, case_input_offset, case_output_offset;
static unsigned int case_flags;

static void check(int condition, const char *expression, int line)
{
  ++assertions;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed (%s, %s, length=%lu, input=%lu, output=%lu, flags=%u)\n",
            __FILE__, line, expression, backend_name, context,
            (unsigned long)case_length, (unsigned long)case_input_offset,
            (unsigned long)case_output_offset, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

enum {
  SIMDURL_TEST_MAX_INPUT = 4096,
  SIMDURL_TEST_ALIGNMENTS = 64,
  SIMDURL_TEST_PADDING = SIMDURL_TEST_ALIGNMENTS * 2
};

static void reference_lower(const char *input, size_t length, char *output)
{
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char value = (unsigned char)input[i];
    output[i] = (char)(value >= 65 && value <= 90 ? value + 32 : value);
  }
}

static void reference_hex(const char *input, size_t length, char *output,
                          unsigned int flags)
{
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned int byte = (unsigned char)input[i];
    unsigned int high = byte / 16, low = byte % 16;
    unsigned int letter = flags == SIMDURL_HEX_UPPER ? 'A' : 'a';
    output[i * 2] = (char)(high < 10 ? '0' + high : letter + high - 10);
    output[i * 2 + 1] = (char)(low < 10 ? '0' + low : letter + low - 10);
  }
}

static void check_result(simdurl_result result, simdurl_status status,
                         size_t written)
{
  CHECK(result.status == status);
  CHECK(result.written == written);
}

static void check_lower_case(lower_function lower, const char *input,
                             size_t length, size_t input_offset,
                             size_t output_offset)
{
  char source[SIMDURL_TEST_MAX_INPUT + SIMDURL_TEST_PADDING], source_before[sizeof(source)];
  char output[SIMDURL_TEST_MAX_INPUT + SIMDURL_TEST_PADDING], expected[sizeof(output)];
  case_length = length;
  case_input_offset = input_offset;
  case_output_offset = output_offset;
  case_flags = 0;
  CHECK(length <= SIMDURL_TEST_MAX_INPUT && input_offset < SIMDURL_TEST_ALIGNMENTS &&
        output_offset < SIMDURL_TEST_ALIGNMENTS);
  memset(source, 0xa5, sizeof(source));
  memcpy(source + input_offset, input, length);
  memcpy(source_before, source, sizeof(source));
  memset(output, 0x5a, sizeof(output));
  memcpy(expected, output, sizeof(output));
  reference_lower(input, length, expected + output_offset);
  check_result(lower(source + input_offset, length, output + output_offset,
                     length), SIMDURL_OK, length);
  CHECK(memcmp(output, expected, sizeof(output)) == 0);
  CHECK(memcmp(source, source_before, sizeof(source)) == 0);
  reference_lower(input, length, source_before + input_offset);
  check_result(lower(source + input_offset, length, source + input_offset,
                     length), SIMDURL_OK, length);
  CHECK(memcmp(source, source_before, sizeof(source)) == 0);
}

static void check_hex_case(hex_function hex, const char *input, size_t length,
                           size_t input_offset, size_t output_offset,
                           unsigned int flags)
{
  char source[SIMDURL_TEST_MAX_INPUT + SIMDURL_TEST_PADDING], source_before[sizeof(source)];
  char output[SIMDURL_TEST_MAX_INPUT * 2 + SIMDURL_TEST_PADDING], expected[sizeof(output)];
  case_length = length;
  case_input_offset = input_offset;
  case_output_offset = output_offset;
  case_flags = flags;
  CHECK(length <= SIMDURL_TEST_MAX_INPUT && input_offset < SIMDURL_TEST_ALIGNMENTS &&
        output_offset < SIMDURL_TEST_ALIGNMENTS);
  memset(source, 0xa5, sizeof(source));
  memcpy(source + input_offset, input, length);
  memcpy(source_before, source, sizeof(source));
  memset(output, 0x5a, sizeof(output));
  memcpy(expected, output, sizeof(output));
  reference_hex(input, length, expected + output_offset, flags);
  check_result(hex(source + input_offset, length, output + output_offset,
                   length * 2, flags), SIMDURL_OK, length * 2);
  CHECK(memcmp(output, expected, sizeof(output)) == 0);
  CHECK(memcmp(source, source_before, sizeof(source)) == 0);
}

#if defined(SIMDURL_HEADER_ONLY)
#define DEFINE_BACKEND_WRAPPERS(suffix) \
  static simdurl_result lower_##suffix(const char *input, size_t length, \
                                       char *output, size_t capacity) \
  { \
    simdurl_result result = { SIMDURL_OK, length }; \
    (void)capacity; \
    simdurl_detail_ascii_lower_##suffix(input, length, output); \
    return result; \
  } \
  static simdurl_result hex_##suffix(const char *input, size_t length, \
                                     char *output, size_t capacity, \
                                     unsigned int flags) \
  { \
    simdurl_result result = { SIMDURL_OK, length * 2 }; \
    (void)capacity; \
    simdurl_detail_hex_encode_##suffix(input, length, output, flags); \
    return result; \
  }
DEFINE_BACKEND_WRAPPERS(portable)
#ifdef SIMDURL_DETAIL_X86
DEFINE_BACKEND_WRAPPERS(sse2)
DEFINE_BACKEND_WRAPPERS(avx2)
#endif
#undef DEFINE_BACKEND_WRAPPERS
#endif

static void test_contract(void)
{
  char output[128], before[sizeof(output)];
  unsigned int flags, unknown;
  size_t length, capacity;
  static const char input[] = { 'A', 'z', 0, '[', '`', (char)128, (char)255 };
  context = "public argument contracts and failure atomicity";
  memset(output, 0x5a, sizeof(output));
  memcpy(before, output, sizeof(output));
  check_result(simdurl_ascii_lower(NULL, 0, NULL, 0), SIMDURL_OK, 0);
  check_result(simdurl_ascii_lower(NULL, 0, output, sizeof(output)), SIMDURL_OK, 0);
  check_result(simdurl_ascii_lower("", 0, NULL, 1), SIMDURL_INVALID_ARGUMENT, 0);
  check_result(simdurl_ascii_lower(NULL, 1, output, sizeof(output)),
               SIMDURL_INVALID_ARGUMENT, 0);
  check_result(simdurl_ascii_lower(NULL, SIZE_MAX, output, sizeof(output)),
               SIMDURL_INVALID_ARGUMENT, 0);
  check_result(simdurl_ascii_lower(input, sizeof(input), NULL, 1),
               SIMDURL_INVALID_ARGUMENT, 0);
  check_result(simdurl_ascii_lower(input, sizeof(input), NULL, 0),
               SIMDURL_BUFFER_TOO_SMALL, 0);
  check_result(simdurl_ascii_lower(input, SIZE_MAX, output, sizeof(output)),
               SIMDURL_BUFFER_TOO_SMALL, 0);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  CHECK(simdurl_hex_encode_bound(0) == 0);
  CHECK(simdurl_hex_encode_bound(1) == 2);
  CHECK(simdurl_hex_encode_bound(SIZE_MAX / 2) == (SIZE_MAX / 2) * 2);
  CHECK(simdurl_hex_encode_bound(SIZE_MAX / 2 + 1) == SIZE_MAX);
  CHECK(simdurl_hex_encode_bound(SIZE_MAX) == SIZE_MAX);
  for(flags = 0; flags <= 1; ++flags) {
    case_flags = flags;
    check_result(simdurl_hex_encode(NULL, 0, NULL, 0, flags), SIMDURL_OK, 0);
    check_result(simdurl_hex_encode(NULL, 0, output, sizeof(output), flags),
                 SIMDURL_OK, 0);
    check_result(simdurl_hex_encode("", 0, NULL, 1, flags),
                 SIMDURL_INVALID_ARGUMENT, 0);
    check_result(simdurl_hex_encode(NULL, 1, output, sizeof(output), flags),
                 SIMDURL_INVALID_ARGUMENT, 0);
    check_result(simdurl_hex_encode(input, 1, NULL, 1, flags),
                 SIMDURL_INVALID_ARGUMENT, 0);
    check_result(simdurl_hex_encode(input, 1, NULL, 0, flags),
                 SIMDURL_BUFFER_TOO_SMALL, 0);
    check_result(simdurl_hex_encode(input, SIZE_MAX / 2 + 1, output,
                                    sizeof(output), flags),
                 SIMDURL_INVALID_ARGUMENT, 0);
    check_result(simdurl_hex_encode(input, SIZE_MAX, output, SIZE_MAX, flags),
                 SIMDURL_INVALID_ARGUMENT, 0);
    check_result(simdurl_hex_encode(input, SIZE_MAX / 2, output,
                                    sizeof(output), flags),
                 SIMDURL_BUFFER_TOO_SMALL, 0);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
  }
  for(unknown = 2; unknown; unknown <<= 1) {
    for(flags = 0; flags <= 1; ++flags) {
      check_result(simdurl_hex_encode(NULL, 0, NULL, 0, unknown | flags),
                   SIMDURL_INVALID_ARGUMENT, 0);
      check_result(simdurl_hex_encode(input, sizeof(input), output,
                                      sizeof(output), unknown | flags),
                   SIMDURL_INVALID_ARGUMENT, 0);
      check_result(simdurl_hex_encode(input, sizeof(input), output, 0,
                                      unknown | flags),
                   SIMDURL_INVALID_ARGUMENT, 0);
      CHECK(memcmp(output, before, sizeof(output)) == 0);
    }
  }
  check_result(simdurl_hex_encode(input, sizeof(input), output, sizeof(output),
                                  UINT_MAX), SIMDURL_INVALID_ARGUMENT, 0);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  for(length = 1; length <= sizeof(input); ++length) {
    case_length = length;
    CHECK(simdurl_hex_encode_bound(length) == length * 2);
    for(capacity = 0; capacity < length; ++capacity) {
      check_result(simdurl_ascii_lower(input, length, output, capacity),
                   SIMDURL_BUFFER_TOO_SMALL, 0);
      CHECK(memcmp(output, before, sizeof(output)) == 0);
    }
    for(capacity = 0; capacity < length * 2; ++capacity) {
      for(flags = 0; flags <= 1; ++flags) {
        check_result(simdurl_hex_encode(input, length, output, capacity, flags),
                     SIMDURL_BUFFER_TOO_SMALL, 0);
        CHECK(memcmp(output, before, sizeof(output)) == 0);
      }
    }
  }
  /* Surplus capacity is not permission to append a terminator or pad stores. */
  check_result(simdurl_ascii_lower(input, sizeof(input), output, sizeof(output)),
               SIMDURL_OK, sizeof(input));
  reference_lower(input, sizeof(input), before);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  for(flags = 0; flags <= 1; ++flags) {
    memset(output, 0x5a, sizeof(output));
    memset(before, 0x5a, sizeof(before));
    reference_hex(input, sizeof(input), before, flags);
    check_result(simdurl_hex_encode(input, sizeof(input), output,
                                    sizeof(output), flags),
                 SIMDURL_OK, sizeof(input) * 2);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
  }
}

static void test_capacities(void)
{
  char input[129], output[260], expected[sizeof(output)];
  size_t length, capacity, i;
  unsigned int flags;
  context = "every output capacity and unchanged destinations on failure";
  for(i = 0; i < sizeof(input); ++i)
    input[i] = (char)(i * 17 + 'A');
  for(length = 0; length <= sizeof(input); ++length) {
    case_length = length;
    for(capacity = 0; capacity <= length + 2; ++capacity) {
      simdurl_status status = capacity < length ? SIMDURL_BUFFER_TOO_SMALL :
                                                SIMDURL_OK;
      size_t written = capacity < length ? 0 : length;
      memset(output, 0xa5, sizeof(output));
      memcpy(expected, output, sizeof(output));
      if(capacity >= length)
        reference_lower(input, length, expected);
      check_result(simdurl_ascii_lower(input, length, output, capacity),
                   status, written);
      CHECK(memcmp(output, expected, sizeof(output)) == 0);
      memcpy(output, input, length);
      memcpy(expected, output, sizeof(output));
      if(capacity >= length)
        reference_lower(input, length, expected);
      check_result(simdurl_ascii_lower(output, length, output, capacity),
                   status, written);
      CHECK(memcmp(output, expected, sizeof(output)) == 0);
    }
    for(capacity = 0; capacity <= length * 2 + 2; ++capacity) {
      for(flags = 0; flags <= 1; ++flags) {
        case_flags = flags;
        memset(output, 0xa5, sizeof(output));
        memcpy(expected, output, sizeof(output));
        if(capacity >= length * 2)
          reference_hex(input, length, expected, flags);
        check_result(simdurl_hex_encode(input, length, output, capacity, flags),
                     capacity < length * 2 ? SIMDURL_BUFFER_TOO_SMALL : SIMDURL_OK,
                     capacity < length * 2 ? 0 : length * 2);
        CHECK(memcmp(output, expected, sizeof(output)) == 0);
      }
    }
  }
}

static void test_all_bytes(lower_function lower, hex_function hex)
{
  static const size_t lengths[] = { 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 129 };
  char input[256];
  unsigned int byte, flags;
  size_t index;
  context = "all byte values across SIMD and tail lanes";
  for(byte = 0; byte < 256; ++byte) {
    memset(input, (int)byte, sizeof(input));
    for(index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
      check_lower_case(lower, input, lengths[index], byte % SIMDURL_TEST_ALIGNMENTS,
                       (byte * 7) % SIMDURL_TEST_ALIGNMENTS);
      for(flags = 0; flags <= 1; ++flags)
        check_hex_case(hex, input, lengths[index], byte % SIMDURL_TEST_ALIGNMENTS,
                       (byte * 7) % SIMDURL_TEST_ALIGNMENTS, flags);
    }
  }
  for(byte = 0; byte < 256; ++byte)
    input[byte] = (char)byte;
  check_lower_case(lower, input, sizeof(input), 0, 0);
  for(flags = 0; flags <= 1; ++flags)
    check_hex_case(hex, input, sizeof(input), 0, 0, flags);
}

static void test_boundaries(lower_function lower, hex_function hex)
{
  char input[257];
  size_t length, offset, position, i;
  unsigned int flags;
  context = "all lengths through 257 and every alignment";
  for(i = 0; i < sizeof(input); ++i)
    input[i] = (char)(i * 67 + 29);
  for(length = 0; length <= sizeof(input); ++length) {
    for(offset = 0; offset < SIMDURL_TEST_ALIGNMENTS; ++offset) {
      check_lower_case(lower, input, length, offset,
                       (offset * 17 + 9) % SIMDURL_TEST_ALIGNMENTS);
      for(flags = 0; flags <= 1; ++flags)
        check_hex_case(hex, input, length, offset,
                       (offset * 17 + 9) % SIMDURL_TEST_ALIGNMENTS, flags);
    }
  }
  context = "one uppercase letter in every block and tail position";
  memset(input, 0xff, sizeof(input));
  for(position = 0; position < sizeof(input); ++position) {
    input[position] = (char)('A' + position % 26);
    check_lower_case(lower, input, sizeof(input), position % SIMDURL_TEST_ALIGNMENTS,
                     (position * 11) % SIMDURL_TEST_ALIGNMENTS);
    check_lower_case(lower, input, position, position % SIMDURL_TEST_ALIGNMENTS,
                     (position * 11) % SIMDURL_TEST_ALIGNMENTS);
    check_lower_case(lower, input, position + 1, position % SIMDURL_TEST_ALIGNMENTS,
                     (position * 11) % SIMDURL_TEST_ALIGNMENTS);
    input[position] = (char)255;
  }
}

static void test_exact_allocations(lower_function lower, hex_function hex)
{
  size_t length, i;
  unsigned int flags;
  context = "exact heap objects for sanitizer bounds checking";
  for(length = 1; length <= 513; ++length) {
    char *input = (char *)malloc(length);
    char *lowered = (char *)malloc(length);
    char *encoded = (char *)malloc(length * 2);
    char expected[1026];
    case_length = length;
    CHECK(input != NULL && lowered != NULL && encoded != NULL);
    for(i = 0; i < length; ++i)
      input[i] = (char)(i * 47 + length);
    reference_lower(input, length, expected);
    check_result(lower(input, length, lowered, length), SIMDURL_OK, length);
    CHECK(memcmp(lowered, expected, length) == 0);
    for(flags = 0; flags <= 1; ++flags) {
      case_flags = flags;
      reference_hex(input, length, expected, flags);
      check_result(hex(input, length, encoded, length * 2, flags),
                   SIMDURL_OK, length * 2);
      CHECK(memcmp(encoded, expected, length * 2) == 0);
    }
    reference_lower(input, length, expected);
    check_result(lower(input, length, input, length), SIMDURL_OK, length);
    CHECK(memcmp(input, expected, length) == 0);
    free(encoded);
    free(lowered);
    free(input);
  }
}

static uint32_t random_word(uint32_t *state)
{
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void test_randomized(lower_function lower, hex_function hex)
{
  uint32_t state = UINT32_C(0x913ba563);
  char input[SIMDURL_TEST_MAX_INPUT];
  size_t trial, length, i;
  unsigned int flags;
  context = "deterministic differential comparison";
  for(trial = 0; trial < 1200; ++trial) {
    length = random_word(&state) % (SIMDURL_TEST_MAX_INPUT + 1);
    for(i = 0; i < length; ++i)
      input[i] = (char)random_word(&state);
    check_lower_case(lower, input, length, trial % SIMDURL_TEST_ALIGNMENTS,
                     (trial * 19) % SIMDURL_TEST_ALIGNMENTS);
    for(flags = 0; flags <= 1; ++flags)
      check_hex_case(hex, input, length, trial % SIMDURL_TEST_ALIGNMENTS,
                     (trial * 19) % SIMDURL_TEST_ALIGNMENTS, flags);
  }
}

#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
typedef struct guarded_span {
  char *pages;
  char *data;
  size_t page_size;
} guarded_span;

static void protect_span(const guarded_span *span, int writable)
{
#if defined(SIMDURL_TEST_POSIX)
  CHECK(mprotect(span->data, span->page_size,
                 writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0);
#else
  DWORD previous;
  CHECK(VirtualProtect(span->data, span->page_size,
                        writable ? PAGE_READWRITE : PAGE_READONLY,
                        &previous) != 0);
#endif
}

static guarded_span allocate_guarded_span(void)
{
  guarded_span span;
#if defined(SIMDURL_TEST_POSIX)
  long size = sysconf(_SC_PAGESIZE);
  CHECK(size > 0);
  span.page_size = (size_t)size;
  span.pages = (char *)mmap(NULL, span.page_size * 3, PROT_NONE,
                            MAP_PRIVATE | SIMDURL_TEST_MAP_ANON, -1, 0);
  CHECK(span.pages != MAP_FAILED);
  span.data = span.pages + span.page_size;
  protect_span(&span, 1);
#else
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  span.page_size = (size_t)info.dwPageSize;
  span.pages = (char *)VirtualAlloc(NULL, span.page_size * 3, MEM_RESERVE,
                                   PAGE_NOACCESS);
  CHECK(span.pages != NULL);
  span.data = (char *)VirtualAlloc(span.pages + span.page_size, span.page_size,
                                  MEM_COMMIT, PAGE_READWRITE);
  CHECK(span.data == span.pages + span.page_size);
#endif
  CHECK(span.page_size >= 514);
  return span;
}

static void free_guarded_span(const guarded_span *span)
{
#if defined(SIMDURL_TEST_POSIX)
  CHECK(munmap(span->pages, span->page_size * 3) == 0);
#else
  CHECK(VirtualFree(span->pages, 0, MEM_RELEASE) != 0);
#endif
}

static void test_guard_pages(lower_function lower, hex_function hex,
                              int public_api)
{
  guarded_span source = allocate_guarded_span();
  guarded_span destination = allocate_guarded_span();
  char expected[514];
  size_t length, input_side, output_side, i;
  unsigned int flags;
  context = "guard pages and read-only source spans";
  check_result(lower(source.pages, 0, destination.pages, 0), SIMDURL_OK, 0);
  for(flags = 0; flags <= 1; ++flags)
    check_result(hex(source.pages, 0, destination.pages, 0, flags), SIMDURL_OK, 0);
  if(public_api) {
    check_result(lower(source.pages, 2, destination.pages, 1),
                 SIMDURL_BUFFER_TOO_SMALL, 0);
    check_result(hex(source.pages, 1, destination.pages, 1, SIMDURL_HEX_LOWER),
                 SIMDURL_BUFFER_TOO_SMALL, 0);
    check_result(hex(source.pages, SIZE_MAX / 2 + 1, destination.pages,
                     SIZE_MAX, SIMDURL_HEX_LOWER), SIMDURL_INVALID_ARGUMENT, 0);
    check_result(hex(source.pages, 1, destination.pages, 2, 2),
                 SIMDURL_INVALID_ARGUMENT, 0);
  }
  for(input_side = 0; input_side < 2; ++input_side) {
    for(length = 0; length <= 257; ++length) {
      char *input = input_side ? source.data + source.page_size - length :
                                source.data;
      case_length = length;
      case_input_offset = input_side;
      for(i = 0; i < length; ++i)
        input[i] = (char)(i * 59 + length);
      protect_span(&source, 0);
      for(output_side = 0; output_side < 2; ++output_side) {
        char *output = output_side ? destination.data + destination.page_size -
                                    length : destination.data;
        case_output_offset = output_side;
        reference_lower(input, length, expected);
        check_result(lower(input, length, output, length), SIMDURL_OK, length);
        CHECK(memcmp(output, expected, length) == 0);
        output = output_side ? destination.data + destination.page_size -
                               length * 2 : destination.data;
        for(flags = 0; flags <= 1; ++flags) {
          case_flags = flags;
          reference_hex(input, length, expected, flags);
          check_result(hex(input, length, output, length * 2, flags),
                       SIMDURL_OK, length * 2);
          CHECK(memcmp(output, expected, length * 2) == 0);
        }
      }
      protect_span(&source, 1);
      reference_lower(input, length, expected);
      check_result(lower(input, length, input, length), SIMDURL_OK, length);
      CHECK(memcmp(input, expected, length) == 0);
    }
  }
  free_guarded_span(&destination);
  free_guarded_span(&source);
}
#endif

static void test_helpers(lower_function lower, hex_function hex,
                          const char *name, int public_api)
{
  backend_name = name;
  test_all_bytes(lower, hex);
  test_boundaries(lower, hex);
  test_exact_allocations(lower, hex);
  test_randomized(lower, hex);
#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
  test_guard_pages(lower, hex, public_api);
#else
  (void)public_api;
#endif
  printf("%s byte helpers passed\n", name);
}

int main(void)
{
  test_contract();
  test_capacities();
  test_helpers(simdurl_ascii_lower, simdurl_hex_encode, "public API", 1);
#if defined(SIMDURL_HEADER_ONLY)
  test_helpers(lower_portable, hex_portable, "portable", 0);
#ifdef SIMDURL_DETAIL_X86
  test_helpers(lower_sse2, hex_sse2, "SSE2", 0);
  if(simdurl_detail_has_avx2())
    test_helpers(lower_avx2, hex_avx2, "AVX2", 0);
#endif
#endif
  printf("simdurl byte helpers: %lu checks passed\n", assertions);
  return EXIT_SUCCESS;
}
