/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
/* Memory-contract checks deliberately use the public API and independent
 * byte-by-byte oracles. No assertion depends on NDEBUG. */
#if defined(SIMDURL_TEST_POSIX)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <simdurl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SIMDURL_TEST_POSIX)
#include <sys/mman.h>
#include <unistd.h>
#if defined(MAP_ANONYMOUS)
#define TEST_MAP_ANON MAP_ANONYMOUS
#else
#define TEST_MAP_ANON MAP_ANON
#endif
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { MAX_INPUT = 8193, ALIGNMENTS = 64, REDZONE = 64, PATTERNS = 8 };
static unsigned long assertions, cases;
static int extended;
static const char *phase = "initialization", *operation = "allocation";
static size_t case_length, case_capacity, input_offset, output_offset;
static unsigned int case_flags, case_pattern;
static int case_in_place, input_side = -1, output_side = -1;
static const unsigned char *case_source;

static void check(int condition, const char *expression, int line)
{
  ++assertions;
  if(!condition) {
    size_t i;
    fprintf(stderr, "%s:%d: %s failed\n"
            "phase=%s operation=%s length=%lu capacity=%lu flags=%u "
            "pattern=%u input_offset=%lu output_offset=%lu in_place=%d "
            "input_guard_side=%d output_guard_side=%d extended=%d\n",
            __FILE__, line, expression, phase, operation,
            (unsigned long)case_length, (unsigned long)case_capacity,
            case_flags, case_pattern, (unsigned long)input_offset,
            (unsigned long)output_offset, case_in_place,
            input_side, output_side, extended);
    if(case_source) {
      fprintf(stderr, "input_prefix_hex=");
      for(i = 0; i < case_length && i < 96; ++i)
        fprintf(stderr, "%02x", (unsigned int)case_source[i]);
      fprintf(stderr, "\n");
    }
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

typedef struct expectation {
  size_t length;
  int rejected;
} expectation;

static int hex_digit(unsigned char c)
{
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static expectation reference(const unsigned char *input, size_t length,
                             unsigned char *output, unsigned int flags,
                             int encode)
{
  static const unsigned char digits[] = "0123456789ABCDEF";
  expectation result = { 0, 0 };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char byte = input[i];
    if(encode) {
      int safe = (byte >= 'a' && byte <= 'z') ||
                 (byte >= 'A' && byte <= 'Z') ||
                 (byte >= '0' && byte <= '9') ||
                 byte == '-' || byte == '_' || byte == '.' ||
                 byte == ((flags & SIMDURL_FORM) ? '*' : '~');
      if(safe)
        output[result.length++] = byte;
      else if(byte == ' ' && (flags & SIMDURL_FORM))
        output[result.length++] = '+';
      else {
        output[result.length++] = '%';
        output[result.length++] = digits[byte / 16];
        output[result.length++] = digits[byte % 16];
      }
    }
    else {
      if(byte == '%' && length - i > 2 &&
         hex_digit(input[i + 1]) >= 0 && hex_digit(input[i + 2]) >= 0) {
        byte = (unsigned char)(hex_digit(input[i + 1]) * 16 +
                               hex_digit(input[i + 2]));
        i += 2;
      }
      else if(byte == '+' && (flags & SIMDURL_FORM))
        byte = ' ';
      if(((flags & SIMDURL_REJECT_NUL) && byte == 0) ||
         ((flags & SIMDURL_REJECT_CONTROL) && byte < 32))
        result.rejected = 1;
      /* Continue after rejection: full length determines whether the capacity
       * and rejection errors overlap, without imposing an error precedence. */
      output[result.length++] = byte;
    }
  }
  return result;
}

static simdurl_status reference_validate(const unsigned char *source,
                                         size_t length, unsigned int flags)
{
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char c = source[i];
    if((c < 32 && (flags & SIMDURL_CHECK_C0)) ||
       (c == 127 && (flags & SIMDURL_CHECK_DEL)) ||
       (c == 32 && (flags & SIMDURL_CHECK_SPACE)))
      return SIMDURL_REJECTED;
  }
  return SIMDURL_OK;
}

static void pattern(unsigned char *buffer, size_t length, unsigned int which)
{
  static const unsigned char literal[] = "abcXYZ019-._~*";
  static const unsigned char escapes[] = "%41%2b%7e%ff%aB";
  static const unsigned char mixed[] = "ab+ cd%20%2B%%4g%a%FF~*";
  static const unsigned char rejected[] = { 'a', 0, 'b', 31, 32, 127, 128, 255 };
  size_t i;
  for(i = 0; i < length; ++i) {
    switch(which) {
    case 0: buffer[i] = literal[i % (sizeof(literal) - 1)]; break;
    case 1: buffer[i] = escapes[i % (sizeof(escapes) - 1)]; break;
    case 2: buffer[i] = mixed[i % (sizeof(mixed) - 1)]; break;
    case 3: buffer[i] = (unsigned char)(i * 73u + 19u); break;
    case 4: buffer[i] = rejected[i % sizeof(rejected)]; break;
    case 5: buffer[i] = 'a'; break;
    case 6: buffer[i] = (unsigned char)"%00%1f%7F"[i % 9]; break;
    default: buffer[i] = (unsigned char)"%41"[i % 3]; break;
    }
  }
  /* Rejections at the very end exercise error exits after full vector stores. */
  if(which == 5 && length) {
    if(length >= 3) memcpy(buffer + length - 3, "%00", 3);
    else buffer[length - 1] = 0;
  }
}

static simdurl_result invoke(const unsigned char *input, size_t length,
                             unsigned char *output, size_t capacity,
                             unsigned int flags, int encode)
{
  ++cases;
  operation = encode ? "encode" : "decode";
  case_length = length;
  case_capacity = capacity;
  case_flags = flags;
  if(encode)
    return simdurl_encode((const char *)input, length, (char *)output,
                           capacity, flags);
  return simdurl_decode((const char *)input, length, (char *)output,
                         capacity, flags);
}

static void check_result(simdurl_result actual, expectation expected,
                          const unsigned char *output,
                          const unsigned char *expected_bytes, size_t capacity)
{
  if(expected.rejected) {
    if(capacity >= expected.length)
      CHECK(actual.status == SIMDURL_REJECTED);
    else
      CHECK(actual.status == SIMDURL_REJECTED ||
            actual.status == SIMDURL_BUFFER_TOO_SMALL);
    CHECK(actual.written == 0);
  }
  else if(capacity < expected.length) {
    CHECK(actual.status == SIMDURL_BUFFER_TOO_SMALL);
    CHECK(actual.written == 0);
  }
  else {
    CHECK(actual.status == SIMDURL_OK);
    CHECK(actual.written == expected.length);
    if(expected.length)
      CHECK(memcmp(output, expected_bytes, expected.length) == 0);
  }
}

static int is_filled(const unsigned char *bytes, size_t length, unsigned char c)
{
  size_t i;
  for(i = 0; i < length; ++i)
    if(bytes[i] != c) return 0;
  return 1;
}

/* Both regions include a whole vector's worth of canaries. For in-place calls,
 * bytes beyond capacity can contain unread input and must retain those bytes. */
static void buffered_case(const unsigned char *source, size_t length,
                           unsigned int flags, int encode, size_t capacity,
                           size_t in_offset, size_t out_offset, int in_place,
                           expectation expected,
                           const unsigned char *expected_bytes)
{
  unsigned char input[MAX_INPUT + ALIGNMENTS + 2 * REDZONE];
  unsigned char output[3 * MAX_INPUT + ALIGNMENTS + 2 * REDZONE + 1];
  unsigned char before[MAX_INPUT + ALIGNMENTS + 2 * REDZONE + 1];
  size_t in_start = REDZONE + in_offset;
  size_t out_start = REDZONE + out_offset;
  size_t input_size = in_start + length + REDZONE;
  size_t output_size = out_start + capacity + REDZONE;
  unsigned char *destination;
  simdurl_result actual;
  case_source = source;
  input_offset = in_offset;
  output_offset = out_offset;
  case_in_place = in_place;
  CHECK(length <= MAX_INPUT && in_offset < ALIGNMENTS && out_offset < ALIGNMENTS);
  memset(input, 0xa5, input_size);
  memcpy(input + in_start, source, length);
  if(in_place) {
    size_t span = length > capacity ? length : capacity;
    CHECK(!encode && capacity <= MAX_INPUT + 1);
    input_size = in_start + span + REDZONE;
    /* Keep actual input untouched while initializing possible extra capacity. */
    memset(input + in_start + length, 0xa5, span - length + REDZONE);
    memcpy(before, input, input_size);
    destination = input + in_start;
    output_offset = in_offset;
  }
  else {
    CHECK(output_size <= sizeof(output));
    memset(output, 0xa5, output_size);
    destination = output + out_start;
  }
  actual = invoke(input + in_start, length, destination, capacity, flags, encode);
  check_result(actual, expected, destination, expected_bytes, capacity);
  if(in_place) {
    CHECK(memcmp(input, before, in_start) == 0);
    CHECK(memcmp(input + in_start + capacity, before + in_start + capacity,
                 input_size - in_start - capacity) == 0);
  }
  else {
    CHECK(is_filled(output, out_start, 0xa5));
    CHECK(is_filled(output + out_start + capacity, REDZONE, 0xa5));
    CHECK(is_filled(input, in_start, 0xa5));
    CHECK(memcmp(input + in_start, source, length) == 0);
    CHECK(is_filled(input + in_start + length, REDZONE, 0xa5));
  }
}

static size_t capacities(size_t *values, size_t length, expectation expected,
                          int encode)
{
  static const size_t thresholds[] = { 15, 16, 17, 31, 32, 33, 63, 64, 65 };
  size_t maximum = encode ? length * 3 : length;
  size_t count = 0, i, j;
  for(i = 0; i < 6 + sizeof(thresholds) / sizeof(thresholds[0]); ++i) {
    size_t value;
    if(i == 0) value = 0;
    else if(i == 1) value = expected.length ? expected.length - 1 : 0;
    else if(i == 2) value = expected.length;
    else if(i == 3) value = maximum ? maximum - 1 : 0;
    else if(i == 4) value = maximum;
    else if(i == 5) value = maximum + 1;
    else value = thresholds[i - 6];
    if(value > maximum + 1) continue;
    for(j = 0; j < count && values[j] != value; ++j) { }
    if(j == count) values[count++] = value;
  }
  return count;
}

static void test_small(void)
{
  unsigned char source[257], expected_bytes[3 * 257];
  size_t length, capacity, choices[16], count, i;
  unsigned int p, flags;
  int encode;
  phase = "small lengths and capacities";
  for(length = 0; length <= 256; ++length) {
    for(p = 0; p < PATTERNS; ++p) {
      case_pattern = p;
      pattern(source, length, p);
      for(encode = 0; encode <= 1; ++encode) {
        for(flags = 0; flags < (encode ? 2u : 8u); ++flags) {
          expectation expected = reference(source, length, expected_bytes, flags, encode);
          if(length <= (extended ? 256u : 32u)) {
            count = (encode ? length * 3 : length) + 2;
          }
          else
            count = capacities(choices, length, expected, encode);
          for(i = 0; i < count; ++i) {
            capacity = length <= (extended ? 256u : 32u) ? i : choices[i];
            buffered_case(source, length, flags, encode, capacity,
                          (length + p * 7u) % ALIGNMENTS,
                          (length * 17u + i * 13u) % ALIGNMENTS, 0,
                          expected, expected_bytes);
            if(!encode)
              buffered_case(source, length, flags, encode, capacity,
                            (length + i * 7u) % ALIGNMENTS, 0, 1,
                            expected, expected_bytes);
          }
        }
      }
    }
  }
}

static void test_alignment(void)
{
  static const size_t normal_lengths[] = { 32, 33, 64, 65, 128, 129 };
  static const size_t extended_lengths[] = {
    15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 96, 97,
    127, 128, 129, 255, 256, 257
  };
  static const unsigned int decode_flags[] = { 0, 1, 6, 7 };
  const size_t *lengths = extended ? extended_lengths : normal_lengths;
  size_t length_count = extended ? sizeof(extended_lengths) / sizeof(size_t) :
                                  sizeof(normal_lengths) / sizeof(size_t);
  unsigned char source[257], expected_bytes[3 * 257];
  size_t l, a, b, c;
  unsigned int p, f, flags;
  int encode;
  phase = "independent input/output alignments modulo 64";
  for(l = 0; l < length_count; ++l) {
    size_t length = lengths[l];
    for(p = 0; p < (extended ? PATTERNS : 3u); ++p) {
      case_pattern = p;
      pattern(source, length, p);
      for(encode = 0; encode <= 1; ++encode) {
        for(f = 0; f < (encode ? 2u : (extended ? 8u : 4u)); ++f) {
          expectation expected;
          size_t caps[4];
          flags = encode || extended ? f : decode_flags[f];
          expected = reference(source, length, expected_bytes, flags, encode);
          caps[0] = expected.length;
          caps[1] = (encode ? length * 3 : length) + 1;
          caps[2] = expected.length ? expected.length - 1 : 0;
          caps[3] = encode ? length * 3 : length;
          for(a = 0; a < ALIGNMENTS; ++a) {
            for(c = 0; c < (extended ? 4u : 2u); ++c) {
              if(!encode)
                buffered_case(source, length, flags, 0, caps[c], a, a, 1,
                              expected, expected_bytes);
              for(b = 0; b < ALIGNMENTS; ++b)
                buffered_case(source, length, flags, encode, caps[c], a, b, 0,
                              expected, expected_bytes);
            }
          }
        }
      }
    }
  }
}

/* malloc has no logical input padding here: ASan can detect even an overread
 * that would otherwise stay within an accessible page. Output allocations end
 * exactly at the advertised capacity, including calls that return errors. */
static void test_exact_allocations(void)
{
  unsigned char source[1024], expected_bytes[3 * 1024];
  size_t length, i, count, choices[16];
  unsigned int p, flags;
  int encode;
  phase = "exact heap objects (sanitizer boundaries)";
  case_in_place = 0;
  input_offset = output_offset = 0;
  for(length = 0; length <= 1024; ++length) {
    unsigned char *input = length ? (unsigned char *)malloc(length) : NULL;
    CHECK(!length || input != NULL);
    for(p = 0; p < PATTERNS; ++p) {
      case_pattern = p;
      pattern(source, length, p);
      case_source = source;
      if(length) memcpy(input, source, length);
      for(flags = 0; flags < 8; ++flags) {
        operation = "validate";
        case_length = length;
        case_capacity = 0;
        case_flags = flags;
        ++cases;
        CHECK(simdurl_validate_bytes((const char *)input, length, flags) ==
              reference_validate(source, length, flags));
      }
      for(encode = 0; encode <= 1; ++encode) {
        for(flags = 0; flags < (encode ? 2u : 8u); ++flags) {
          expectation expected = reference(source, length, expected_bytes, flags, encode);
          count = capacities(choices, length, expected, encode);
          for(i = 0; i < count; ++i) {
            size_t capacity = choices[i];
            unsigned char *output = capacity ? (unsigned char *)malloc(capacity) : NULL;
            simdurl_result actual;
            CHECK(!capacity || output != NULL);
            /* Leave destination bytes uninitialized: MSan can detect an
             * accidental dependence on their previous contents. Only a
             * successful written prefix is compared below. */
            actual = invoke(input, length, output, capacity, flags, encode);
            check_result(actual, expected, output, expected_bytes, capacity);
            if(length) CHECK(memcmp(input, source, length) == 0);
            free(output);
            if(!encode) {
              size_t span = length > capacity ? length : capacity;
              unsigned char *shared = span ? (unsigned char *)malloc(span) : NULL;
              CHECK(!span || shared != NULL);
              if(span) memset(shared, 0xa5, span);
              if(length) memcpy(shared, source, length);
              case_in_place = 1;
              actual = invoke(shared, length, shared, capacity, flags, 0);
              check_result(actual, expected, shared, expected_bytes, capacity);
              if(capacity < length)
                CHECK(memcmp(shared + capacity, source + capacity,
                             length - capacity) == 0);
              case_in_place = 0;
              free(shared);
            }
          }
        }
      }
    }
    free(input);
  }
  case_source = NULL;
}

#if defined(SIMDURL_TEST_POSIX) || defined(_WIN32)
typedef struct guarded_region {
  unsigned char *mapping, *bytes;
  size_t page_size, size;
} guarded_region;

static void set_writable(guarded_region *region, int writable)
{
#if defined(SIMDURL_TEST_POSIX)
  CHECK(mprotect(region->bytes, region->size,
                 writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0);
#else
  DWORD previous;
  CHECK(VirtualProtect(region->bytes, region->size,
                        writable ? PAGE_READWRITE : PAGE_READONLY, &previous) != 0);
#endif
}

static guarded_region new_region(size_t minimum)
{
  guarded_region result;
#if defined(SIMDURL_TEST_POSIX)
  long page_size = sysconf(_SC_PAGESIZE);
  CHECK(page_size > 0);
  result.page_size = (size_t)page_size;
#else
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  result.page_size = (size_t)info.dwPageSize;
  CHECK(result.page_size != 0);
#endif
  result.size = ((minimum + result.page_size - 1) / result.page_size) * result.page_size;
#if defined(SIMDURL_TEST_POSIX)
  result.mapping = (unsigned char *)mmap(NULL, result.size + result.page_size * 2,
      PROT_NONE, MAP_PRIVATE | TEST_MAP_ANON, -1, 0);
  CHECK(result.mapping != MAP_FAILED);
  result.bytes = result.mapping + result.page_size;
  set_writable(&result, 1);
#else
  result.mapping = (unsigned char *)VirtualAlloc(NULL,
      result.size + result.page_size * 2, MEM_RESERVE, PAGE_NOACCESS);
  CHECK(result.mapping != NULL);
  result.bytes = (unsigned char *)VirtualAlloc(result.mapping + result.page_size,
      result.size, MEM_COMMIT, PAGE_READWRITE);
  CHECK(result.bytes == result.mapping + result.page_size);
#endif
  return result;
}

static void free_region(guarded_region *region)
{
#if defined(SIMDURL_TEST_POSIX)
  CHECK(munmap(region->mapping, region->size + region->page_size * 2) == 0);
#else
  CHECK(VirtualFree(region->mapping, 0, MEM_RELEASE) != 0);
#endif
}

static unsigned char *place(guarded_region *region, size_t span, int trailing)
{
  CHECK(span <= region->size);
  return trailing ? region->bytes + region->size - span : region->bytes;
}

static void test_guard_pages(void)
{
  guarded_region in = new_region(MAX_INPUT + 1);
  guarded_region out = new_region(3 * MAX_INPUT + 1);
  unsigned char source[MAX_INPUT], expected_bytes[3 * MAX_INPUT];
  unsigned char before[MAX_INPUT + 1];
  size_t lengths[280], length_count = 0, l, i, count, choices[16];
  unsigned int p, flags;
  int a, b, encode;
  phase = "protected short circuits";
  case_source = NULL;
  case_length = case_capacity = 0;
  for(flags = 0; flags < 8; ++flags) {
    CHECK(simdurl_validate_bytes((const char *)in.mapping, 0, flags) == SIMDURL_OK);
    CHECK(simdurl_decode((const char *)in.mapping, 0, (char *)out.mapping,
                         0, flags).status == SIMDURL_OK);
  }
  CHECK(simdurl_validate_bytes((const char *)in.mapping, in.page_size, 0) == SIMDURL_OK);
  CHECK(simdurl_validate_bytes((const char *)in.mapping, in.page_size, 8) ==
        SIMDURL_INVALID_ARGUMENT);
  for(flags = 0; flags < 2; ++flags)
    CHECK(simdurl_encode((const char *)in.mapping, 0, (char *)out.mapping,
                         0, flags).status == SIMDURL_OK);
  CHECK(simdurl_encode((const char *)in.mapping, 1, (char *)out.mapping,
                       1, 8).status == SIMDURL_INVALID_ARGUMENT);
  CHECK(simdurl_decode((const char *)in.mapping, 1, (char *)out.mapping,
                       1, 8).status == SIMDURL_INVALID_ARGUMENT);
  CHECK(simdurl_encode((const char *)in.mapping, SIZE_MAX / 3 + 1,
                       (char *)out.mapping, 1, 0).status == SIMDURL_INVALID_ARGUMENT);
  for(i = 0; i <= 257; ++i) lengths[length_count++] = i;
  lengths[length_count++] = 511;
  lengths[length_count++] = 512;
  lengths[length_count++] = 513;
  lengths[length_count++] = 1023;
  lengths[length_count++] = 1024;
  lengths[length_count++] = 1025;
  if(in.page_size <= MAX_INPUT) {
    lengths[length_count++] = in.page_size - 1;
    lengths[length_count++] = in.page_size;
    if(in.page_size < MAX_INPUT) lengths[length_count++] = in.page_size + 1;
  }
  lengths[length_count++] = MAX_INPUT;
  for(l = 0; l < length_count; ++l) {
    size_t length = lengths[l];
    for(p = 0; p < PATTERNS; ++p) {
      case_pattern = p;
      case_source = source;
      pattern(source, length, p);
      for(a = 0; a < 2; ++a) {
        unsigned char *input = place(&in, length, a);
        input_side = a;
        case_in_place = 0;
        phase = "read-only input with leading/trailing protected pages";
        if(length) memcpy(input, source, length);
        set_writable(&in, 0);
        for(flags = 0; flags < 8; ++flags) {
          operation = "validate";
          case_length = length;
          case_capacity = 0;
          case_flags = flags;
          ++cases;
          CHECK(simdurl_validate_bytes((const char *)input, length, flags) ==
                reference_validate(source, length, flags));
        }
        for(encode = 0; encode <= 1; ++encode) {
          for(flags = 0; flags < (encode ? 2u : 8u); ++flags) {
            expectation expected = reference(source, length, expected_bytes, flags, encode);
            count = capacities(choices, length, expected, encode);
            for(b = 0; b < 2; ++b) {
              output_side = b;
              for(i = 0; i < count; ++i) {
                size_t capacity = choices[i];
                unsigned char *output = place(&out, capacity, b);
                size_t prefix = b ? REDZONE : 0;
                size_t suffix = b ? 0 : REDZONE;
                simdurl_result actual;
                memset(output - prefix, 0xa5, prefix + capacity + suffix);
                input_offset = (size_t)((uintptr_t)input % ALIGNMENTS);
                output_offset = (size_t)((uintptr_t)output % ALIGNMENTS);
                actual = invoke(input, length, output, capacity, flags, encode);
                check_result(actual, expected, output, expected_bytes, capacity);
                CHECK(is_filled(output - prefix, prefix, 0xa5));
                CHECK(is_filled(output + capacity, suffix, 0xa5));
              }
            }
          }
        }
        set_writable(&in, 1);
      }
      phase = "in-place contraction at protected page boundaries";
      case_in_place = 1;
      for(flags = 0; flags < 8; ++flags) {
        expectation expected = reference(source, length, expected_bytes, flags, 0);
        count = capacities(choices, length, expected, 0);
        for(a = 0; a < 2; ++a) {
          input_side = output_side = a;
          for(i = 0; i < count; ++i) {
            size_t capacity = choices[i];
            size_t span = length > capacity ? length : capacity;
            unsigned char *buffer = place(&in, span, a);
            simdurl_result actual;
            memset(buffer, 0xa5, span);
            if(length) memcpy(buffer, source, length);
            memcpy(before, buffer, span);
            if(!a) memset(buffer + span, 0xa5, REDZONE);
            else memset(buffer - REDZONE, 0xa5, REDZONE);
            input_offset = output_offset = (size_t)((uintptr_t)buffer % ALIGNMENTS);
            actual = invoke(buffer, length, buffer, capacity, flags, 0);
            check_result(actual, expected, buffer, expected_bytes, capacity);
            CHECK(memcmp(buffer + capacity, before + capacity, span - capacity) == 0);
            CHECK(is_filled(a ? buffer - REDZONE : buffer + span, REDZONE, 0xa5));
          }
        }
      }
    }
  }
  input_side = output_side = -1;
  case_source = NULL;
  free_region(&in);
  free_region(&out);
  printf("guard pages: executed (leading, trailing, read-only input, error paths)\n");
}
#else
static void test_guard_pages(void)
{
  printf("guard pages: SKIPPED (platform protection APIs unavailable)\n");
}
#endif

int main(int argc, char **argv)
{
  if(argc == 2 && strcmp(argv[1], "--extended") == 0)
    extended = 1;
  else if(argc != 1) {
    fprintf(stderr, "usage: %s [--extended]\n", argv[0]);
    return EXIT_FAILURE;
  }
  test_small();
  test_alignment();
  test_exact_allocations();
  test_guard_pages();
  printf("simdurl memory: %lu cases, %lu checks passed (%s)\n", cases,
         assertions, extended ? "extended" : "normal");
  return EXIT_SUCCESS;
}
