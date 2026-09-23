/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SIMDURL_TEST_POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Deliberately independent of assert(): Release builds test the same checks. */
static unsigned long checks;
static const char *context = "initialization";
static size_t case_length;
static unsigned int case_flags;

static void check(int condition, const char *expression, int line)
{
  ++checks;
  if(!condition) {
    fprintf(stderr, "%s:%d: %s failed (%s, length=%lu, flags=%u)\n",
            __FILE__, line, expression, context,
            (unsigned long)case_length, case_flags);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

enum {
  SIMDURL_TEST_MAX_INPUT = 1024,
  SIMDURL_TEST_STORAGE = SIMDURL_TEST_MAX_INPUT * 3 + 160
};

static unsigned int hex_value(unsigned char c)
{
  if(c >= '0' && c <= '9')
    return (unsigned int)(c - '0');
  if(c >= 'a' && c <= 'f')
    return (unsigned int)(c - 'a') + 10;
  if(c >= 'A' && c <= 'F')
    return (unsigned int)(c - 'A') + 10;
  return 256;
}

static size_t reference_encode(const unsigned char *input, size_t length,
                               unsigned char *output, unsigned int flags)
{
  static const char alphabet[] = "0123456789ABCDEF";
  size_t i, written = 0;
  for(i = 0; i < length; ++i) {
    unsigned char c = input[i];
    int unchanged = (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' ||
                    c == ((flags & SIMDURL_FORM) ? '*' : '~');
    if(unchanged)
      output[written++] = c;
    else if(c == ' ' && (flags & SIMDURL_FORM))
      output[written++] = '+';
    else {
      output[written++] = '%';
      output[written++] = (unsigned char)alphabet[c >> 4];
      output[written++] = (unsigned char)alphabet[c & 15];
    }
  }
  return written;
}

static simdurl_result reference_decode(const unsigned char *input, size_t length,
                                      unsigned char *output, unsigned int flags)
{
  simdurl_result result = { SIMDURL_OK, 0 };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char c = input[i];
    if(c == '%' && length - i >= 3 &&
       hex_value(input[i + 1]) < 16 && hex_value(input[i + 2]) < 16) {
      c = (unsigned char)((hex_value(input[i + 1]) << 4) |
                          hex_value(input[i + 2]));
      i += 2;
    }
    else if(c == '+' && (flags & SIMDURL_FORM))
      c = ' ';
    if(((flags & SIMDURL_REJECT_NUL) && c == 0) ||
       ((flags & SIMDURL_REJECT_CONTROL) && c < 0x20)) {
      result.status = SIMDURL_REJECTED;
      result.written = 0;
      return result;
    }
    output[result.written++] = c;
  }
  return result;
}

static void check_canaries(const unsigned char *storage, size_t offset,
                           size_t capacity)
{
  size_t i;
  for(i = 0; i < offset; ++i)
    CHECK(storage[i] == 0xa5);
  /* A full cache line after capacity catches narrow and SIMD overstores. */
  for(i = offset + capacity; i < offset + capacity + 64; ++i)
    CHECK(storage[i] == 0xa5);
}

static void check_case(const unsigned char *source, size_t length,
                       unsigned int flags, int encode)
{
  unsigned char input[SIMDURL_TEST_MAX_INPUT + 64];
  unsigned char expected[SIMDURL_TEST_MAX_INPUT * 3];
  unsigned char storage[SIMDURL_TEST_STORAGE];
  simdurl_result reference;
  size_t capacities[5], offset, i, capacity;
  case_length = length;
  case_flags = flags;
  CHECK(length <= SIMDURL_TEST_MAX_INPUT);
  offset = 1 + (length % 31);
  memcpy(input + offset, source, length);
  if(encode) {
    reference.status = SIMDURL_OK;
    reference.written = reference_encode(source, length, expected, flags);
  }
  else
    reference = reference_decode(source, length, expected, flags);
  if(reference.status == SIMDURL_REJECTED) {
    simdurl_result actual;
    memset(storage, 0xa5, sizeof(storage));
    actual = simdurl_decode((const char *)input + offset, length,
                            (char *)storage + offset, length + 32, flags);
    CHECK(actual.status == SIMDURL_REJECTED);
    CHECK(actual.written == 0);
    check_canaries(storage, offset, length + 32);
    return;
  }
  capacities[0] = 0;
  capacities[1] = reference.written ? reference.written - 1 : 0;
  capacities[2] = reference.written;
  capacities[3] = encode ? length * 3 : length;
  capacities[4] = capacities[3] + 32;
  for(i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
    simdurl_result actual;
    capacity = capacities[i];
    memset(storage, 0xa5, sizeof(storage));
    if(encode)
      actual = simdurl_encode((const char *)input + offset, length,
                              (char *)storage + offset, capacity, flags);
    else
      actual = simdurl_decode((const char *)input + offset, length,
                              (char *)storage + offset, capacity, flags);
    if(capacity < reference.written) {
      CHECK(actual.status == SIMDURL_BUFFER_TOO_SMALL);
      CHECK(actual.written == 0);
    }
    else {
      CHECK(actual.status == SIMDURL_OK);
      CHECK(actual.written == reference.written);
      CHECK(memcmp(storage + offset, expected, actual.written) == 0);
    }
    check_canaries(storage, offset, capacity);
    CHECK(memcmp(input + offset, source, length) == 0);
  }
}

static void check_in_place(const unsigned char *source, size_t length,
                           unsigned int flags)
{
  unsigned char buffer[SIMDURL_TEST_MAX_INPUT + 128];
  unsigned char before[SIMDURL_TEST_MAX_INPUT + 128];
  unsigned char expected[SIMDURL_TEST_MAX_INPUT];
  simdurl_result reference = reference_decode(source, length, expected, flags);
  size_t variant;
  case_length = length;
  case_flags = flags;
  for(variant = 0; variant < 2; ++variant) {
    const size_t offset = 17;
    size_t capacity = variant ? length : reference.written;
    simdurl_result actual;
    if(reference.status != SIMDURL_OK)
      capacity = length;
    memset(buffer, 0xa5, sizeof(buffer));
    memcpy(buffer + offset, source, length);
    memcpy(before, buffer, sizeof(buffer));
    actual = simdurl_decode((char *)buffer + offset, length,
                            (char *)buffer + offset, capacity, flags);
    CHECK(actual.status == reference.status);
    CHECK(actual.written == reference.written);
    if(reference.status == SIMDURL_OK)
      CHECK(memcmp(buffer + offset, expected, actual.written) == 0);
    CHECK(memcmp(buffer, before, offset) == 0);
    CHECK(memcmp(buffer + offset + capacity, before + offset + capacity,
                 sizeof(buffer) - offset - capacity) == 0);
  }
}

static void test_contract(void)
{
  char output[32];
  simdurl_result result;
  unsigned int invalid[] = { 8u, 16u, UINT_MAX };
  size_t i;
  context = "API contract";
  CHECK(simdurl_encode_bound(0) == 0);
  CHECK(simdurl_encode_bound(1) == 3);
  CHECK(simdurl_encode_bound(SIZE_MAX / 3) == (SIZE_MAX / 3) * 3);
  CHECK(simdurl_encode_bound(SIZE_MAX / 3 + 1) == SIZE_MAX);
  CHECK(simdurl_encode_bound(SIZE_MAX) == SIZE_MAX);
  result = simdurl_encode(NULL, 0, NULL, 0, SIMDURL_URI);
  CHECK(result.status == SIMDURL_OK && result.written == 0);
  result = simdurl_decode(NULL, 0, NULL, 0, SIMDURL_FORM);
  CHECK(result.status == SIMDURL_OK && result.written == 0);
  result = simdurl_encode(NULL, 1, output, sizeof(output), 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_decode(NULL, 1, output, sizeof(output), 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_encode("x", 1, NULL, 1, 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_decode("x", 1, NULL, 1, 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_encode(NULL, 0, NULL, 1, 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_decode(NULL, 0, NULL, 1, 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  result = simdurl_encode("x", 1, NULL, 0, 0);
  CHECK(result.status == SIMDURL_BUFFER_TOO_SMALL && result.written == 0);
  result = simdurl_decode("x", 1, NULL, 0, 0);
  CHECK(result.status == SIMDURL_BUFFER_TOO_SMALL && result.written == 0);
  result = simdurl_encode("x", SIZE_MAX / 3 + 1, output, sizeof(output), 0);
  CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  for(i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    result = simdurl_encode("x", 1, output, sizeof(output), invalid[i]);
    CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
    result = simdurl_decode("x", 1, output, sizeof(output), invalid[i]);
    CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  }
  for(i = 2; i < 8; ++i) {
    result = simdurl_encode("x", 1, output, sizeof(output), (unsigned int)i);
    CHECK(result.status == SIMDURL_INVALID_ARGUMENT && result.written == 0);
  }
  memset(output, 0xa5, sizeof(output));
  result = simdurl_encode("a", 1, output, 1, 0);
  CHECK(result.status == SIMDURL_OK && result.written == 1);
  CHECK(output[0] == 'a' && (unsigned char)output[1] == 0xa5);
  result = simdurl_decode("%00", 3, output, 1, 0);
  CHECK(result.status == SIMDURL_OK && result.written == 1);
  CHECK(output[0] == '\0' && (unsigned char)output[1] == 0xa5);
}

static void test_examples(void)
{
  static const char *const examples[] = {
    "", "plainAZaz09-._~*", "a+b c", "http://example.test/a?b=c&d=e#f",
    "%", "%0", "%GG", "%0g", "%g0", "%%41", "%41%", "%41%4",
    "%2b+%2B%20", "%25%32%42", "%00%01%1f%20%7f%ff", "%%%%",
    "%4%41", "space and unicode: \xc3\xa9 \xf0\x9f\x8c\x8d"
  };
  static const unsigned char binary[] = { 'a', 0, 'b', '+', 0xff, '%', '0', '0' };
  size_t i;
  unsigned int flags;
  context = "examples";
  for(i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
    size_t length = strlen(examples[i]);
    for(flags = 0; flags < 8; ++flags) {
      check_case((const unsigned char *)examples[i], length, flags, 0);
      check_in_place((const unsigned char *)examples[i], length, flags);
      if(flags < 2)
        check_case((const unsigned char *)examples[i], length, flags, 1);
    }
  }
  for(flags = 0; flags < 8; ++flags) {
    check_case(binary, sizeof(binary), flags, 0);
    check_in_place(binary, sizeof(binary), flags);
    if(flags < 2)
      check_case(binary, sizeof(binary), flags, 1);
  }
}

static void test_byte_values(void)
{
  unsigned char input[256];
  unsigned int value, flags;
  context = "all byte values";
  for(value = 0; value < 256; ++value) {
    input[value] = (unsigned char)value;
    for(flags = 0; flags < 8; ++flags) {
      check_case(input + value, 1, flags, 0);
      if(flags < 2)
        check_case(input + value, 1, flags, 1);
    }
  }
  for(flags = 0; flags < 2; ++flags) {
    check_case(input, sizeof(input), flags, 1);
    check_case(input, sizeof(input), flags, 0);
    check_in_place(input, sizeof(input), flags);
  }
  /* Repeat each byte across complete vector blocks, including signed bytes. */
  for(value = 0; value < 256; ++value) {
    memset(input, (int)value, sizeof(input));
    for(flags = 0; flags < 2; ++flags) {
      check_case(input, sizeof(input), flags, 1);
      check_case(input, sizeof(input), flags, 0);
    }
  }
}

static void test_escape_pairs(void)
{
  unsigned char input[3] = { '%', 0, 0 }, expected[128], output[128];
  unsigned char vector_input[128];
  unsigned int first, second, flags;
  memset(vector_input, 'a', sizeof(vector_input));
  context = "all 65536 escape pairs";
  case_length = 3;
  for(first = 0; first < 256; ++first) {
    input[1] = (unsigned char)first;
    for(second = 0; second < 256; ++second) {
      input[2] = (unsigned char)second;
      for(flags = 0; flags < 2; ++flags) {
        simdurl_result reference, actual;
        case_flags = flags;
        reference = reference_decode(input, 3, expected, flags);
        actual = simdurl_decode((const char *)input, 3, (char *)output, 3, flags);
        CHECK(actual.status == reference.status);
        CHECK(actual.written == reference.written);
        CHECK(memcmp(output, expected, actual.written) == 0);
        /* Exercise vector classification and escapes crossing SIMD lanes. */
        memcpy(vector_input, input, 3);
        memcpy(vector_input + 15, input, 3);
        memcpy(vector_input + 31, input, 3);
        memcpy(vector_input + 62, input, 3);
        reference = reference_decode(vector_input, sizeof(vector_input), expected, flags);
        actual = simdurl_decode((const char *)vector_input, sizeof(vector_input),
                                (char *)output, sizeof(output), flags);
        CHECK(actual.status == reference.status);
        CHECK(actual.written == reference.written);
        CHECK(memcmp(output, expected, actual.written) == 0);
      }
    }
  }
}

static void test_vector_boundaries(void)
{
  static const char *const tokens[] = {
    "%41", "%ff", "%2B", "%00", "%1f", "%7F", "%G0", "%0g", "%%0", "+"
  };
  unsigned char input[SIMDURL_TEST_MAX_INPUT];
  size_t length, lane, token;
  unsigned int flags;
  context = "vector boundaries and lanes";
  for(length = 0; length <= 160; ++length) {
    memset(input, 'A', length);
    for(flags = 0; flags < 2; ++flags) {
      check_case(input, length, flags, 1);
      check_case(input, length, flags, 0);
    }
  }
  for(token = 0; token < sizeof(tokens) / sizeof(tokens[0]); ++token) {
    for(lane = 0; lane < 160; ++lane) {
      memset(input, 'q', sizeof(input));
      memcpy(input + lane, tokens[token], strlen(tokens[token]));
      for(flags = 0; flags < 8; ++flags) {
        check_case(input, 192, flags, 0);
        check_in_place(input, 192, flags);
      }
    }
  }
  /* Dense escapes straddle each lane and leave zero, one or two tail bytes. */
  for(lane = 0; lane < 32; ++lane) {
    memset(input, 'x', sizeof(input));
    for(length = lane; length + 3 <= sizeof(input); length += 3)
      memcpy(input + length, "%aF", 3);
    for(length = 127; length <= 161; ++length) {
      check_case(input, length, 0, 0);
      check_in_place(input, length, 0);
    }
  }
  /* Long literal runs cross the decoder's four-vector fast path. */
  memset(input, 'z', sizeof(input));
  for(lane = 0; lane < 160; ++lane) {
    input[384 + lane] = '%';
    input[385 + lane] = '2';
    input[386 + lane] = 'B';
    check_case(input, sizeof(input), SIMDURL_FORM, 0);
    check_in_place(input, sizeof(input), SIMDURL_FORM);
    memset(input + 384 + lane, 'z', 3);
  }
}


static void test_rejection_lanes(void)
{
  static const unsigned char values[] = { 0, 1, 31, 32, 127, 128, 255 };
  unsigned char input[512];
  size_t lane, value;
  unsigned int flags;
  context = "raw rejection across SIMD blocks and literal runs";
  for(lane = 0; lane < 192; ++lane) {
    for(value = 0; value < sizeof(values); ++value) {
      memset(input, 'a', sizeof(input));
      input[256 + lane] = values[value];
      for(flags = 2; flags < 8; ++flags) {
        check_case(input, sizeof(input), flags, 0);
        check_in_place(input, sizeof(input), flags);
      }
    }
  }
}

static uint32_t random_state = UINT32_C(0x42ea1f09);

static uint32_t next_random(void)
{
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static void test_randomized(void)
{
  static const unsigned char alphabet[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~* +%:/";
  unsigned char input[SIMDURL_TEST_MAX_INPUT];
  unsigned char encoded[SIMDURL_TEST_MAX_INPUT * 3];
  unsigned char decoded[SIMDURL_TEST_MAX_INPUT];
  size_t iteration, length, i;
  unsigned int flags;
  context = "deterministic randomized reference comparison";
  for(iteration = 0; iteration < 800; ++iteration) {
    length = next_random() % (SIMDURL_TEST_MAX_INPUT + 1);
    for(i = 0; i < length; ++i) {
      uint32_t value = next_random();
      input[i] = iteration % 3 ? alphabet[value % (sizeof(alphabet) - 1)]
                               : (unsigned char)value;
    }
    for(flags = 0; flags < 2; ++flags) {
      simdurl_result encode_result, decode_result;
      check_case(input, length, flags, 1);
      check_case(input, length, flags, 0);
      check_in_place(input, length, flags);
      encode_result = simdurl_encode((const char *)input, length, (char *)encoded,
                                     sizeof(encoded), flags);
      CHECK(encode_result.status == SIMDURL_OK);
      decode_result = simdurl_decode((const char *)encoded, encode_result.written,
                                     (char *)decoded, sizeof(decoded), flags);
      CHECK(decode_result.status == SIMDURL_OK);
      CHECK(decode_result.written == length);
      CHECK(memcmp(decoded, input, length) == 0);
    }
    if(iteration % 8 == 0) {
      for(flags = 2; flags < 8; ++flags)
        check_case(input, length, flags, 0);
    }
  }
}

#if defined(SIMDURL_TEST_POSIX)
static void test_guard_pages(void)
{
  long page_size_value = sysconf(_SC_PAGESIZE);
  size_t page_size, length, pattern;
  unsigned char *source_pages, *output_pages;
  int descriptor = open("/dev/zero", O_RDWR);
  context = "guard pages";
  CHECK(page_size_value > 0 && descriptor >= 0);
  page_size = (size_t)page_size_value;
  source_pages = (unsigned char *)mmap(NULL, page_size * 2,
    PROT_READ | PROT_WRITE, MAP_PRIVATE, descriptor, 0);
  output_pages = (unsigned char *)mmap(NULL, page_size * 2,
    PROT_READ | PROT_WRITE, MAP_PRIVATE, descriptor, 0);
  CHECK(source_pages != MAP_FAILED && output_pages != MAP_FAILED);
  CHECK(close(descriptor) == 0);
  CHECK(mprotect(source_pages + page_size, page_size, PROT_NONE) == 0);
  CHECK(mprotect(output_pages + page_size, page_size, PROT_NONE) == 0);
  for(length = 0; length <= 192; ++length) {
    unsigned char *source = source_pages + page_size - length;
    unsigned int flags;
    case_length = length;
    for(pattern = 0; pattern < 4; ++pattern) {
      unsigned char expected[SIMDURL_TEST_MAX_INPUT * 3];
      size_t i;
      for(i = 0; i < length; ++i) {
        if(pattern == 0)
          source[i] = 'a';
        else if(pattern == 1)
          source[i] = (unsigned char)"%41+"[i % 4];
        else if(pattern == 2)
          source[i] = (unsigned char)(i * 197);
        else
          source[i] = (unsigned char)"%q0"[i % 3];
      }
      for(flags = 0; flags < 2; ++flags) {
        simdurl_result reference, actual;
        size_t encoded_length = reference_encode(source, length, expected, flags);
        unsigned char *destination = output_pages + page_size - encoded_length;
        case_flags = flags;
        actual = simdurl_encode((const char *)source, length, (char *)destination,
                                encoded_length, flags);
        CHECK(actual.status == SIMDURL_OK && actual.written == encoded_length);
        CHECK(memcmp(destination, expected, encoded_length) == 0);
        destination = output_pages + page_size - length * 3;
        actual = simdurl_encode((const char *)source, length, (char *)destination,
                                length * 3, flags);
        CHECK(actual.status == SIMDURL_OK && actual.written == encoded_length);
        CHECK(memcmp(destination, expected, encoded_length) == 0);
        reference = reference_decode(source, length, expected, flags);
        destination = output_pages + page_size - reference.written;
        actual = simdurl_decode((const char *)source, length, (char *)destination,
                                reference.written, flags);
        CHECK(actual.status == reference.status && actual.written == reference.written);
        CHECK(memcmp(destination, expected, actual.written) == 0);
        destination = output_pages + page_size - length;
        actual = simdurl_decode((const char *)source, length, (char *)destination,
                                length, flags);
        CHECK(actual.status == reference.status && actual.written == reference.written);
        CHECK(memcmp(destination, expected, actual.written) == 0);
      }
    }
  }
  CHECK(munmap(source_pages, page_size * 2) == 0);
  CHECK(munmap(output_pages, page_size * 2) == 0);
}
#endif

int main(void)
{
  test_contract();
  test_examples();
  test_byte_values();
  test_escape_pairs();
  test_vector_boundaries();
  test_rejection_lanes();
  test_randomized();
#if defined(SIMDURL_TEST_POSIX)
  test_guard_pages();
#endif
  printf("simdurl: %lu checks passed\n", checks);
  return EXIT_SUCCESS;
}
