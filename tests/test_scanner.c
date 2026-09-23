/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
/* Expose anonymous mappings in strict C99 test builds before libc headers. */
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

typedef simdurl_status (*validate_function)(const char *, size_t, unsigned int);

/* Keep assertions active in Release builds. */
static unsigned long assertions, backend_cases;
static validate_function current_validate;
static const char *context = "initialization";
static const char *backend_name = "public API";
static size_t case_length, case_offset;
static unsigned int case_flags;

static void check(int condition, const char *expression, int line)
{
  ++assertions;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed (%s, %s, length=%lu, offset=%lu, checks=%u)\n",
            __FILE__, line, expression, backend_name, context,
            (unsigned long)case_length, (unsigned long)case_offset, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

enum { SIMDURL_TEST_MAX_INPUT = 4096, SIMDURL_TEST_ALIGNMENTS = 64 };

static simdurl_status reference_validate(const char *input, size_t length,
                                         unsigned int flags)
{
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char byte = (unsigned char)input[i];
    if((byte < 32 && (flags & SIMDURL_CHECK_C0)) ||
       (byte == 127 && (flags & SIMDURL_CHECK_DEL)) ||
       (byte == 32 && (flags & SIMDURL_CHECK_SPACE)))
      return SIMDURL_REJECTED;
  }
  return SIMDURL_OK;
}

static void check_case(validate_function validate, const char *input,
                       size_t length, unsigned int flags, size_t offset)
{
  char storage[SIMDURL_TEST_MAX_INPUT + SIMDURL_TEST_ALIGNMENTS * 2];
  char before[sizeof(storage)];
  simdurl_status expected;
  case_length = length;
  case_offset = offset;
  case_flags = flags;
  CHECK(length <= SIMDURL_TEST_MAX_INPUT && offset < SIMDURL_TEST_ALIGNMENTS);
  memset(storage, 0xa5, sizeof(storage));
  memcpy(storage + offset, input, length);
  memcpy(before, storage, sizeof(storage));
  expected = reference_validate(input, length, flags);
  CHECK(validate(storage + offset, length, flags) == expected);
  CHECK(memcmp(storage, before, sizeof(storage)) == 0);
}

#if defined(SIMDURL_HEADER_ONLY)
/* Direct checks exercise fallback kernels even when dispatch selects AVX2.
 * These internal function names are deliberately not part of the public API. */
static simdurl_status validate_portable(const char *input, size_t length,
                                       unsigned int flags)
{
  return simdurl_detail_scan_portable(input, length, flags) ?
         SIMDURL_REJECTED : SIMDURL_OK;
}
#ifdef SIMDURL_DETAIL_X86
static simdurl_status validate_sse2(const char *input, size_t length,
                                   unsigned int flags)
{
  return simdurl_detail_scan_sse2(input, length, flags) ?
         SIMDURL_REJECTED : SIMDURL_OK;
}

static simdurl_status validate_avx2(const char *input, size_t length,
                                   unsigned int flags)
{
  return simdurl_detail_scan_avx2(input, length, flags) ?
         SIMDURL_REJECTED : SIMDURL_OK;
}
#endif
#endif

static void test_contract(void)
{
  unsigned int flags, unknown;
  static const char binary[] = { 'a', 0, 'b', 127, (char)128, (char)255 };
  static const char escaped[] = "%00%1f%20%7F+%FF";
  context = "public contract";
  for(flags = 0; flags < 8; ++flags) {
    case_flags = flags;
    CHECK(simdurl_validate_bytes(NULL, 0, flags) == SIMDURL_OK);
    CHECK(simdurl_validate_bytes("", 0, flags) == SIMDURL_OK);
    CHECK(simdurl_validate_bytes(NULL, 1, flags) == SIMDURL_INVALID_ARGUMENT);
    CHECK(simdurl_validate_bytes(NULL, SIZE_MAX, flags) == SIMDURL_INVALID_ARGUMENT);
    CHECK(simdurl_validate_bytes(escaped, sizeof(escaped) - 1, flags) == SIMDURL_OK);
    CHECK(simdurl_validate_bytes(binary, sizeof(binary), flags) ==
          reference_validate(binary, sizeof(binary), flags));
  }
  for(unknown = 8; unknown; unknown <<= 1) {
    for(flags = 0; flags < 8; ++flags) {
      case_flags = unknown | flags;
      CHECK(simdurl_validate_bytes(NULL, 0, case_flags) == SIMDURL_INVALID_ARGUMENT);
      CHECK(simdurl_validate_bytes("", 0, case_flags) == SIMDURL_INVALID_ARGUMENT);
      CHECK(simdurl_validate_bytes("a", 1, case_flags) == SIMDURL_INVALID_ARGUMENT);
    }
  }
  CHECK(simdurl_validate_bytes("a", 1, UINT_MAX) == SIMDURL_INVALID_ARGUMENT);
  CHECK(simdurl_validate_bytes(binary, sizeof(binary), 0) == SIMDURL_OK);
  /* The explicit length can stop before a rejected byte. */
  CHECK(simdurl_validate_bytes(binary, 1, SIMDURL_CHECK_C0) == SIMDURL_OK);
  CHECK(simdurl_validate_bytes(binary, 2, SIMDURL_CHECK_C0) == SIMDURL_REJECTED);
  /* An embedded NUL must not stop a scan selecting only DEL. */
  CHECK(simdurl_validate_bytes(binary, sizeof(binary), SIMDURL_CHECK_DEL) ==
        SIMDURL_REJECTED);
}

static void test_all_bytes(validate_function validate)
{
  static const size_t lengths[] = {
    1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129
  };
  char input[129];
  unsigned int byte, flags;
  size_t index;
  context = "all 256 byte values and eight check combinations";
  for(byte = 0; byte < 256; ++byte) {
    memset(input, (int)byte, sizeof(input));
    for(index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
      for(flags = 0; flags < 8; ++flags)
        check_case(validate, input, lengths[index], flags,
                   byte % SIMDURL_TEST_ALIGNMENTS);
    }
  }
}

static void test_boundaries(validate_function validate)
{
  static const unsigned char values[] = { 0, 1, 31, 32, 33, 126, 127, 128, 255 };
  char input[257];
  size_t length, offset, position, value;
  unsigned int flags;
  context = "SIMD lengths and every input alignment";
  memset(input, 0xff, sizeof(input));
  for(length = 0; length <= 256; ++length) {
    for(offset = 0; offset < SIMDURL_TEST_ALIGNMENTS; ++offset) {
      for(flags = 0; flags < 8; ++flags)
        check_case(validate, input, length, flags, offset);
    }
  }
  context = "selected byte in every vector and tail lane";
  for(position = 0; position < sizeof(input); ++position) {
    for(value = 0; value < sizeof(values); ++value) {
      input[position] = (char)values[value];
      for(flags = 0; flags < 8; ++flags) {
        check_case(validate, input, sizeof(input), flags,
                   position % SIMDURL_TEST_ALIGNMENTS);
        check_case(validate, input, position, flags,
                   position % SIMDURL_TEST_ALIGNMENTS);
        check_case(validate, input, position + 1, flags,
                   position % SIMDURL_TEST_ALIGNMENTS);
      }
    }
    input[position] = (char)255;
  }
}

/* Exact allocations let address sanitizers catch reads outside the object,
 * including overreads that would remain within an accessible memory page. */
static void test_exact_allocations(validate_function validate)
{
  size_t length, pattern;
  unsigned int flags;
  context = "exact input allocations";
  for(length = 1; length <= 1024; ++length) {
    char *input = (char *)malloc(length);
    case_length = length;
    case_offset = 0;
    CHECK(input != NULL);
    memset(input, 0xff, length);
    for(pattern = 0; pattern < 4; ++pattern) {
      input[length - 1] = pattern == 0 ? (char)255 :
                         (pattern == 1 ? 0 : (pattern == 2 ? 32 : 127));
      for(flags = 0; flags < 8; ++flags) {
        case_flags = flags;
        CHECK(validate(input, length, flags) ==
              reference_validate(input, length, flags));
      }
    }
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

static void test_randomized(validate_function validate)
{
  uint32_t state = UINT32_C(0x6a592e43);
  char input[SIMDURL_TEST_MAX_INPUT];
  size_t trial, length, i;
  unsigned int flags;
  context = "deterministic differential comparison";
  for(trial = 0; trial < 1200; ++trial) {
    length = random_word(&state) % (SIMDURL_TEST_MAX_INPUT + 1);
    for(i = 0; i < length; ++i) {
      unsigned char byte = (unsigned char)random_word(&state);
      if(trial % 3 != 0) {
        if(byte <= 32)
          byte = (unsigned char)(byte + 33);
        if(byte == 127)
          byte = 128;
      }
      input[i] = (char)byte;
    }
    if(trial % 3 == 2 && length)
      input[random_word(&state) % length] = (char)random_word(&state);
    for(flags = 0; flags < 8; ++flags)
      check_case(validate, input, length, flags, trial % SIMDURL_TEST_ALIGNMENTS);
  }
}

#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
static int protect_page(char *page, size_t length, int writable)
{
#if defined(SIMDURL_TEST_POSIX)
  return mprotect(page, length, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0;
#else
  DWORD previous;
  return VirtualProtect(page, length, writable ? PAGE_READWRITE : PAGE_READONLY,
                        &previous) != 0;
#endif
}

static void test_guard_pages(validate_function validate, int public_api)
{
  char *pages, *middle;
  size_t page_size, length, side, pattern;
  unsigned int flags;
  context = "read-only spans between inaccessible guard pages";
#if defined(SIMDURL_TEST_POSIX)
  {
    long value = sysconf(_SC_PAGESIZE);
    CHECK(value > 0);
    page_size = (size_t)value;
  }
  pages = (char *)mmap(NULL, page_size * 3, PROT_NONE,
                      MAP_PRIVATE | SIMDURL_TEST_MAP_ANON, -1, 0);
  CHECK(pages != MAP_FAILED);
  middle = pages + page_size;
  CHECK(protect_page(middle, page_size, 1));
#else
  {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    page_size = (size_t)info.dwPageSize;
  }
  pages = (char *)VirtualAlloc(NULL, page_size * 3, MEM_RESERVE, PAGE_NOACCESS);
  CHECK(pages != NULL);
  middle = (char *)VirtualAlloc(pages + page_size, page_size, MEM_COMMIT,
                                PAGE_READWRITE);
  CHECK(middle == pages + page_size);
#endif
  /* No byte may be read when no checks are requested. */
  CHECK(validate(pages, page_size, 0) == SIMDURL_OK);
  for(flags = 0; flags < 8; ++flags)
    CHECK(validate(pages, 0, flags) == SIMDURL_OK);
  if(public_api)
    CHECK(validate(pages, page_size, 8) == SIMDURL_INVALID_ARGUMENT);
  CHECK(page_size >= 257);
  for(side = 0; side < 2; ++side) {
    for(length = 0; length <= 257; ++length) {
      char *input = side ? middle + page_size - length : middle;
      case_length = length;
      case_offset = side;
      for(pattern = 0; pattern < 4; ++pattern) {
        memset(input, pattern == 0 ? 0xff : 'a', length);
        if(length && pattern > 0)
          input[length - 1] = pattern == 1 ? 0 : (pattern == 2 ? 32 : 127);
        CHECK(protect_page(middle, page_size, 0));
        for(flags = 0; flags < 8; ++flags) {
          simdurl_status expected = reference_validate(input, length, flags);
          case_flags = flags;
          CHECK(validate(input, length, flags) == expected);
        }
        CHECK(protect_page(middle, page_size, 1));
      }
    }
  }
#if defined(SIMDURL_TEST_POSIX)
  CHECK(munmap(pages, page_size * 3) == 0);
#else
  CHECK(VirtualFree(pages, 0, MEM_RELEASE) != 0);
#endif
}
#endif

static simdurl_status counted_validate(const char *input, size_t length,
                                        unsigned int flags)
{
  ++backend_cases;
  return current_validate(input, length, flags);
}

static void test_scanner(validate_function implementation, const char *name,
                         int public_api)
{
  validate_function validate = counted_validate;
  backend_name = name;
  current_validate = implementation;
  backend_cases = 0;
  test_all_bytes(validate);
  test_boundaries(validate);
  test_exact_allocations(validate);
  test_randomized(validate);
#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
  test_guard_pages(validate, public_api);
#else
  (void)public_api;
#endif
  printf("SIMDURL_BACKEND operation=scan backend=%s compiled=1 executed=1 "
         "skipped=0 cases=%lu\n", name, backend_cases);
#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
  printf("SIMDURL_MEMORY operation=scan backend=%s guard_pages=executed\n", name);
#else
  printf("SIMDURL_MEMORY operation=scan backend=%s guard_pages=unavailable\n", name);
#endif
}

static void report_skipped(const char *name, int compiled)
{
  printf("SIMDURL_BACKEND operation=scan backend=%s compiled=%d executed=0 "
         "skipped=1 cases=0 reason=%s\n", name, compiled,
         compiled ? "cpu_unsupported" : "not_compiled");
}

int main(int argc, char **argv)
{
  unsigned int required = 0, available = 0;
  int arg, portable = 0, compiled_x86 = 0, avx2 = 0;
  for(arg = 1; arg < argc; ++arg) {
    if(strcmp(argv[arg], "--require=portable") == 0)
      required |= 1;
    else if(strcmp(argv[arg], "--require=sse2") == 0)
      required |= 2;
    else if(strcmp(argv[arg], "--require=avx2") == 0)
      required |= 4;
    else {
      fprintf(stderr, "Usage: %s [--require=portable] [--require=sse2] "
              "[--require=avx2]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }
  test_contract();
  test_scanner(simdurl_validate_bytes, "public", 1);
#if defined(SIMDURL_HEADER_ONLY)
  portable = 1;
  available |= 1;
  test_scanner(validate_portable, "portable", 0);
#ifdef SIMDURL_DETAIL_X86
  compiled_x86 = 1;
  available |= 2;
  test_scanner(validate_sse2, "sse2", 0);
  avx2 = simdurl_detail_has_avx2();
  if(avx2) {
    available |= 4;
    test_scanner(validate_avx2, "avx2", 0);
  }
#endif
#endif
  if(!portable)
    report_skipped("portable", 0);
  if(!compiled_x86)
    report_skipped("sse2", 0);
  if(!avx2)
    report_skipped("avx2", compiled_x86);
  if(required & ~available) {
    fprintf(stderr, "Required scanner backend did not execute:");
    if((required & 1) && !(available & 1))
      fputs(" portable", stderr);
    if((required & 2) && !(available & 2))
      fputs(" sse2", stderr);
    if((required & 4) && !(available & 4))
      fputs(" avx2", stderr);
    fputc('\n', stderr);
    return EXIT_FAILURE;
  }
  printf("simdurl scanner: %lu checks passed\n", assertions);
  return EXIT_SUCCESS;
}
