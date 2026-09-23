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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SIMDURL_TEST_COUNT_SCANS
/* Include libc declarations first, then instrument only the implementation's
 * memchr calls. Measure logical search distance: a match ends the search even
 * when the supplied limit covers a long suffix. Counting that entire limit
 * would wrongly label repeated nearby matches as quadratic work. The original
 * repeated suffix search still counts in full when its marker is absent.
 * This avoids elapsed-time thresholds and permits different libc backends. */
static size_t scan_work;
static void *counted_memchr(const void *input, int byte, size_t length)
{
  void *match = memchr(input, byte, length);
  size_t searched = match ?
    (size_t)((const char *)match - (const char *)input) + 1 : length;
  if(searched > SIZE_MAX - scan_work) {
    fputs("scan-work counter overflow\n", stderr);
    exit(EXIT_FAILURE);
  }
  scan_work += searched;
  return match;
}
#define memchr counted_memchr
#endif
#include <simdurl.h>
#ifdef SIMDURL_TEST_COUNT_SCANS
#undef memchr
#endif

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

static unsigned long assertions;
static const char *context = "initialization";
static size_t case_length, case_offset;
static unsigned int case_flags;

static void check(int condition, const char *expression, int line)
{
  ++assertions;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed (%s, length=%lu, offset=%lu, flags=%u)\n",
            __FILE__, line, expression, context, (unsigned long)case_length,
            (unsigned long)case_offset, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

enum { SIMDURL_TEST_INPUT = 4096, SIMDURL_TEST_PADDING = 64 };

static unsigned int reference_nibble(unsigned char value)
{
  if(value >= '0' && value <= '9')
    return value - '0';
  if(value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  if(value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return 16;
}

static simdurl_result reference_decode(const char *input, size_t length,
                                       char *output, unsigned int flags)
{
  simdurl_result result = { SIMDURL_OK, 0 };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char value = (unsigned char)input[i];
    if(value == '%' && length - i >= 3) {
      unsigned int high = reference_nibble((unsigned char)input[i + 1]);
      unsigned int low = reference_nibble((unsigned char)input[i + 2]);
      if(high < 16 && low < 16) {
        value = (unsigned char)(high * 16 + low);
        i += 2;
      }
    }
    else if(value == '+' && (flags & SIMDURL_FORM))
      value = ' ';
    if(((flags & SIMDURL_REJECT_NUL) && value == 0) ||
       ((flags & SIMDURL_REJECT_CONTROL) && value < 32)) {
      result.status = SIMDURL_REJECTED;
      result.written = 0;
      return result;
    }
    output[result.written++] = (char)value;
  }
  return result;
}

#if defined(SIMDURL_HEADER_ONLY)
static size_t reference_literals(const char *input, size_t length,
                                 unsigned int form)
{
  size_t i;
  for(i = 0; i < length; ++i)
    if(input[i] == '%' || (form && input[i] == '+'))
      return i;
  return length;
}

static void check_literals(const char *input, size_t length)
{
  unsigned int form;
  for(form = 0; form <= 1; ++form) {
    const char *next_percent = input;
    CHECK(simdurl_detail_literal_length(input, length, form, &next_percent) ==
          reference_literals(input, length, form));
  }
}

static void check_cache_progress(const char *input, size_t length)
{
  static const size_t steps[] = { 1, 32, 128, 257 };
  size_t step_index;
  for(step_index = 0; step_index < sizeof(steps) / sizeof(steps[0]); ++step_index) {
    const char *next_percent = input;
    size_t offset = 0;
    /* One-byte progression visits a cached marker itself, then invalidates it.
     * Larger jumps model a vector block consuming several markers at once. */
    while(offset < length) {
      size_t remaining = length - offset;
      size_t percent = reference_literals(input + offset, remaining, 0);
      case_offset = offset;
      CHECK(simdurl_detail_literal_length(input + offset, remaining, 1,
                                          &next_percent) ==
            reference_literals(input + offset, remaining, 1));
      CHECK(next_percent == input + offset + percent);
      offset += remaining < steps[step_index] ? remaining : steps[step_index];
    }
    CHECK(simdurl_detail_literal_length(input + length, 0, 1, &next_percent) == 0);
  }
}

static void test_cache_progress(void)
{
  static const char mixed[] = "a+%2B+b%q0+c%+d%%41+e%";
  static const char binary[] = { 'a', 0, (char)128, '+', (char)255, '+', 'b' };
  char input[1024];
  size_t i;
  context = "cached percent progression and end sentinel";
  check_cache_progress("", 0);
  check_cache_progress("literal", 7);
  check_cache_progress("a+b+c+d", 7);
  check_cache_progress(mixed, sizeof(mixed) - 1);
  check_cache_progress(binary, sizeof(binary));
  memset(input, 'x', sizeof(input));
  for(i = 31; i < sizeof(input); i += 32)
    input[i] = i % 64 == 31 ? '+' : '%';
  case_length = sizeof(input);
  check_cache_progress(input, sizeof(input));
  memset(input, 'x', sizeof(input));
  for(i = 31; i < sizeof(input); i += 32)
    input[i] = '+';
  check_cache_progress(input, sizeof(input));
}
#endif

static void check_canaries(const char *storage, size_t offset, size_t capacity,
                            size_t storage_size)
{
  size_t i;
  unsigned int changed = 0;
  for(i = 0; i < offset; ++i)
    changed |= (unsigned char)storage[i] ^ 0xa5U;
  for(i = offset + capacity; i < storage_size; ++i)
    changed |= (unsigned char)storage[i] ^ 0xa5U;
  CHECK(changed == 0);
}

static void check_case(const char *input, size_t length, size_t offset)
{
  char source[SIMDURL_TEST_INPUT + SIMDURL_TEST_PADDING];
  char before[sizeof(source)], output[sizeof(source)];
  char inplace[sizeof(source)], expected[SIMDURL_TEST_INPUT];
  unsigned int flags;
  case_length = length;
  case_offset = offset;
  CHECK(length <= SIMDURL_TEST_INPUT && offset < SIMDURL_TEST_PADDING);
  memset(source, 0xa5, sizeof(source));
  memcpy(source + offset, input, length);
  memcpy(before, source, sizeof(source));
#if defined(SIMDURL_HEADER_ONLY)
  check_literals(source + offset, length);
#endif
  for(flags = 0; flags < 8; ++flags) {
    simdurl_result reference = reference_decode(input, length, expected, flags);
    simdurl_result actual;
    case_flags = flags;
    memset(output, 0xa5, sizeof(output));
    actual = simdurl_decode(source + offset, length, output + offset, length, flags);
    CHECK(actual.status == reference.status && actual.written == reference.written);
    if(actual.status == SIMDURL_OK)
      CHECK(memcmp(output + offset, expected, actual.written) == 0);
    check_canaries(output, offset, length, sizeof(output));
    CHECK(memcmp(source, before, sizeof(source)) == 0);
    memcpy(inplace, source, sizeof(source));
    actual = simdurl_decode(inplace + offset, length, inplace + offset, length, flags);
    CHECK(actual.status == reference.status && actual.written == reference.written);
    if(actual.status == SIMDURL_OK)
      CHECK(memcmp(inplace + offset, expected, actual.written) == 0);
    check_canaries(inplace, offset, length, sizeof(inplace));
    if(reference.status == SIMDURL_OK) {
      memset(output, 0xa5, sizeof(output));
      actual = simdurl_decode(source + offset, length, output + offset,
                              reference.written, flags);
      CHECK(actual.status == SIMDURL_OK && actual.written == reference.written);
      CHECK(memcmp(output + offset, expected, actual.written) == 0);
      check_canaries(output, offset, reference.written, sizeof(output));
      if(reference.written) {
        memset(output, 0xa5, sizeof(output));
        actual = simdurl_decode(source + offset, length, output + offset,
                                reference.written - 1, flags);
        CHECK(actual.status == SIMDURL_BUFFER_TOO_SMALL && actual.written == 0);
        check_canaries(output, offset, reference.written - 1, sizeof(output));
      }
    }
  }
}

static void fill_pattern(char *input, size_t length, unsigned int pattern)
{
  static const char *const patterns[] = {
    "literal", "a+", "b%41", "ab+cd%2Bef%00+gh%FF", "xyz%q0+%1+%",
    "\200A\377\0"
  };
  static const size_t sizes[] = {
    sizeof("literal") - 1, sizeof("a+") - 1, sizeof("b%41") - 1,
    sizeof("ab+cd%2Bef%00+gh%FF") - 1, sizeof("xyz%q0+%1+%") - 1,
    sizeof("\200A\377\0") - 1
  };
  size_t i;
  for(i = 0; i < length; ++i)
    input[i] = patterns[pattern][i % sizes[pattern]];
}

static void test_distributions(void)
{
  char input[SIMDURL_TEST_INPUT];
  size_t length, offset;
  unsigned int pattern;
  context = "short and long literal/plus/percent distributions";
  for(pattern = 0; pattern < 6; ++pattern) {
    for(length = 0; length <= 320; ++length) {
      fill_pattern(input, length, pattern);
      check_case(input, length, length % SIMDURL_TEST_PADDING);
    }
    for(length = 1024; length <= sizeof(input); length *= 2) {
      fill_pattern(input, length, pattern);
      check_case(input, length, pattern * 7);
    }
  }
  context = "marker positions across all alignments and vector boundaries";
  memset(input, 'x', sizeof(input));
  for(length = 0; length <= 257; ++length) {
    for(offset = 0; offset < SIMDURL_TEST_PADDING; ++offset) {
      input[length] = '+';
      check_case(input, length + 1, offset);
      input[length] = '%';
      check_case(input, length + 1, offset);
      input[length] = 'x';
    }
  }
  context = "long literals with markers around the 128-byte fast path";
  for(length = 120; length <= 136; ++length) {
    memset(input, 'x', sizeof(input));
    memcpy(input + length, "+%41+%00%q0%1", 13);
    check_case(input, 512, length % SIMDURL_TEST_PADDING);
  }
}

static void test_cached_marker_decoding(void)
{
  static const char suffix[] = "a+%2B+b%q0+c%+d%%41+e%";
  static const char decoded_suffix[] = "a + b%q0 c% d%A e%";
  static const size_t prefixes[] = { 0, 63, 128, 129, 256 };
  char input[320], output[320], expected[320];
  size_t index;
  context = "literal prefixes, consumed percents, escaped plus, and malformed escapes";
  for(index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
    size_t prefix = prefixes[index];
    size_t length = prefix + sizeof(suffix) - 1;
    size_t decoded_length = prefix + sizeof(decoded_suffix) - 1;
    simdurl_result actual;
    memset(input, 'A', prefix);
    memcpy(input + prefix, suffix, sizeof(suffix) - 1);
    memset(expected, 'A', prefix);
    memcpy(expected + prefix, decoded_suffix, sizeof(decoded_suffix) - 1);
    check_case(input, length, index);
    case_flags = SIMDURL_FORM;
    actual = simdurl_decode(input, length, output, sizeof(output), SIMDURL_FORM);
    CHECK(actual.status == SIMDURL_OK && actual.written == decoded_length);
    CHECK(memcmp(output, expected, decoded_length) == 0);
    actual = simdurl_decode(input, length, input, length, SIMDURL_FORM);
    CHECK(actual.status == SIMDURL_OK && actual.written == decoded_length);
    CHECK(memcmp(input, expected, decoded_length) == 0);
  }
}

static void test_exact_allocations(void)
{
  size_t length;
  context = "exact source allocations and NUL/high-byte literals";
  for(length = 1; length <= 513; ++length) {
    char *input = (char *)malloc(length);
    CHECK(input != NULL);
    fill_pattern(input, length, 5);
    if(length > 2)
      input[length - 2] = length % 2 ? '+' : '%';
#if defined(SIMDURL_HEADER_ONLY)
    check_literals(input, length);
#endif
    {
      char *output = (char *)malloc(length);
      char expected[513];
      simdurl_result actual, reference;
      CHECK(output != NULL);
      reference = reference_decode(input, length, expected, SIMDURL_FORM);
      actual = simdurl_decode(input, length, output, length, SIMDURL_FORM);
      CHECK(actual.status == reference.status && actual.written == reference.written);
      CHECK(memcmp(output, expected, actual.written) == 0);
      free(output);
    }
    free(input);
  }
}

#ifdef SIMDURL_TEST_COUNT_SCANS
static void test_linear_scan_work(void)
{
  char input[SIMDURL_TEST_INPUT], output[sizeof(input)], expected[sizeof(input)];
  size_t length;
  unsigned int pattern;
  context = "linear bound on logical libc search work";
  case_flags = SIMDURL_FORM;
  for(length = 1024; length <= sizeof(input); length *= 2) {
    size_t largest_work = 0;
    for(pattern = 0; pattern < 6; ++pattern) {
      simdurl_result actual, reference;
      case_length = length;
      case_offset = pattern;
      fill_pattern(input, length, pattern % 3 + 1);
      if(pattern == 3)
        input[length - 1] = '%';
      else if(pattern == 4)
        input[length - 1] = '+';
      reference = reference_decode(input, length, expected, SIMDURL_FORM);
      scan_work = 0;
      actual = simdurl_decode(input, length, output, length, SIMDURL_FORM);
      CHECK(actual.status == reference.status && actual.written == reference.written);
      CHECK(memcmp(output, expected, actual.written) == 0);
      if(scan_work > length * 4)
        fprintf(stderr, "logical scan bytes: %lu for %lu input bytes\n",
                (unsigned long)scan_work, (unsigned long)length);
      CHECK(scan_work <= length * 4);
      if(scan_work > largest_work)
        largest_work = scan_work;
    }
    printf("literal scans: at most %lu searched bytes for %lu input bytes\n",
           (unsigned long)largest_work, (unsigned long)length);
  }
}

static void test_growth_scan_work(void)
{
  /* Exercise both growing windows and restarts after long literal runs. The
   * input sizes and run lengths are independent of the implementation's
   * initial window; a doubled input must not quadruple the logical search work. */
  static const size_t run_lengths[] = {
    63, 64, 65, 191, 192, 193, 447, 448, 449
  };
  char input[16384], output[sizeof(input)], expected[sizeof(input)];
  size_t distribution, length;
  context = "scan work after growing windows and long literal runs";
  case_flags = SIMDURL_FORM;
  for(distribution = 0;
      distribution <= 2 * (sizeof(run_lengths) / sizeof(run_lengths[0]));
      ++distribution) {
    size_t previous_work = 0;
    for(length = 4096; length <= sizeof(input); length *= 2) {
      simdurl_result actual, reference;
      case_length = length;
      case_offset = distribution;
      memset(input, 'x', length);
      if(distribution) {
        size_t run = run_lengths[(distribution - 1) / 2], position;
        char marker = distribution % 2 ? '+' : '%';
        for(position = run; position < length; position += run + 1)
          input[position] = marker;
        /* Keep the opposite marker far away. The percent-separated form uses
         * non-hex literals, so these markers remain literal percent bytes. */
        input[length - 1] = marker == '+' ? '%' : '+';
      }
      reference = reference_decode(input, length, expected, SIMDURL_FORM);
      scan_work = 0;
      actual = simdurl_decode(input, length, output, length, SIMDURL_FORM);
      CHECK(actual.status == reference.status && actual.written == reference.written);
      CHECK(memcmp(output, expected, actual.written) == 0);
      if(length > 4096) {
        size_t limit = previous_work * 3 + 1024;
        if(scan_work > limit)
          fprintf(stderr, "scan work grew from %lu to %lu for %lu input bytes "
                  "(distribution %lu)\n", (unsigned long)previous_work,
                  (unsigned long)scan_work, (unsigned long)length,
                  (unsigned long)distribution);
        CHECK(scan_work <= limit);
      }
      previous_work = scan_work;
    }
  }
  puts("literal scan work grows linearly for long runs and marker-free input");
}
#endif

#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
static void set_page_access(char *page, size_t length, int writable)
{
#if defined(SIMDURL_TEST_POSIX)
  CHECK(mprotect(page, length, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0);
#else
  DWORD previous;
  CHECK(VirtualProtect(page, length, writable ? PAGE_READWRITE : PAGE_READONLY,
                        &previous) != 0);
#endif
}

static void test_guard_pages(void)
{
  char *pages, *middle;
  size_t page_size, length, side;
  unsigned int pattern;
  context = "bounded loads from read-only guarded inputs";
#if defined(SIMDURL_TEST_POSIX)
  long value = sysconf(_SC_PAGESIZE);
  CHECK(value > 0);
  page_size = (size_t)value;
  pages = (char *)mmap(NULL, page_size * 3, PROT_NONE,
                       MAP_PRIVATE | SIMDURL_TEST_MAP_ANON, -1, 0);
  CHECK(pages != MAP_FAILED);
  middle = pages + page_size;
  set_page_access(middle, page_size, 1);
#else
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  page_size = (size_t)info.dwPageSize;
  pages = (char *)VirtualAlloc(NULL, page_size * 3, MEM_RESERVE, PAGE_NOACCESS);
  CHECK(pages != NULL);
  middle = (char *)VirtualAlloc(pages + page_size, page_size, MEM_COMMIT,
                                PAGE_READWRITE);
  CHECK(middle == pages + page_size);
#endif
  CHECK(page_size >= 513);
  for(side = 0; side < 2; ++side) {
    for(length = 0; length <= 513; ++length) {
      char *input = side ? middle + page_size - length : middle;
      case_length = length;
      case_offset = side;
      for(pattern = 0; pattern < 6; ++pattern) {
        char output[513], expected[513];
        simdurl_result actual, reference;
        fill_pattern(input, length, pattern);
        set_page_access(middle, page_size, 0);
#if defined(SIMDURL_HEADER_ONLY)
        check_literals(input, length);
#endif
        reference = reference_decode(input, length, expected, SIMDURL_FORM);
        actual = simdurl_decode(input, length, output, length, SIMDURL_FORM);
        CHECK(actual.status == reference.status && actual.written == reference.written);
        CHECK(memcmp(output, expected, actual.written) == 0);
        set_page_access(middle, page_size, 1);
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

int main(void)
{
#ifdef SIMDURL_TEST_COUNT_SCANS
  test_linear_scan_work();
  test_growth_scan_work();
#endif
#if defined(SIMDURL_HEADER_ONLY)
  test_cache_progress();
#endif
  test_distributions();
  test_cached_marker_decoding();
  test_exact_allocations();
#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
  test_guard_pages();
#endif
  printf("simdurl literal scanning: %lu checks passed\n", assertions);
  return EXIT_SUCCESS;
}
