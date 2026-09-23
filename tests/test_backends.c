/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#include <simdurl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal kernels deliberately bypass public CPU dispatch. Only call SIMD
 * functions after checking CPU support. These internal names are not API. */
typedef void (*encode_backend)(const char **, size_t *, char **, unsigned int);
typedef int (*decode_backend)(const char **, size_t *, char **, unsigned char,
                              unsigned int, int);

static unsigned long checks, cases, kernel_calls;
static const char *backend_name, *context;
static const char *case_input;
static size_t case_length, source_offset, output_offset;
static unsigned int case_flags;
static int case_in_place;

/* Keep the test limit distinct from the POSIX MAX_INPUT macro. */
enum { ALIGNMENTS = 64, SIMDURL_TEST_MAX_INPUT = 2048, GUARD_SIZE = 32 };

static void check(int condition, const char *expression, int line)
{
  ++checks;
  if(!condition) {
    size_t i;
    fprintf(stderr, "%s:%d: %s failed (%s, %s, length=%lu, flags=%u, "
            "source_offset=%lu, output_offset=%lu, in_place=%d, case=%lu)\n",
            __FILE__, line, expression, backend_name, context,
            (unsigned long)case_length, case_flags,
            (unsigned long)source_offset, (unsigned long)output_offset,
            case_in_place, cases);
    fputs("input_hex=", stderr);
    for(i = 0; i < case_length; ++i)
      fprintf(stderr, "%02x", (unsigned char)case_input[i]);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
  }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

static void record_case(const char *input, size_t length, unsigned int flags,
                         size_t source, size_t output, int in_place)
{
  ++cases;
  case_input = input;
  case_length = length;
  case_flags = flags;
  source_offset = source;
  output_offset = output;
  case_in_place = in_place;
}

/* malloc need not be 64-byte aligned. Establish a known alignment before
 * applying the requested independent offsets, so every residue is exercised. */
static size_t aligned_prefix(const char *storage, size_t offset)
{
  size_t residue = (size_t)(((uintptr_t)storage + GUARD_SIZE) % ALIGNMENTS);
  return GUARD_SIZE + (ALIGNMENTS - residue) % ALIGNMENTS + offset;
}

static void check_canaries(const char *storage, size_t prefix, size_t capacity)
{
  size_t i;
  for(i = 0; i < prefix; ++i)
    CHECK((unsigned char)storage[i] == 0x5a);
  for(i = prefix + capacity; i < prefix + capacity + GUARD_SIZE; ++i)
    CHECK((unsigned char)storage[i] == 0x5a);
}

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

static int reference_hex(unsigned char c)
{
  if(c >= '0' && c <= '9')
    return c - '0';
  if(c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if(c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static simdurl_result reference_decode(const char *input, size_t length,
                                       char *output, unsigned int flags)
{
  simdurl_result result = { SIMDURL_OK, 0 };
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)input[i];
    if(c == '%' && length - i >= 3) {
      int high = reference_hex((unsigned char)input[i + 1]);
      int low = reference_hex((unsigned char)input[i + 2]);
      if(high >= 0 && low >= 0) {
        c = (unsigned char)(high * 16 + low);
        i += 2;
      }
    }
    else if(c == '+' && (flags & SIMDURL_FORM))
      c = ' ';
    if((c == 0 && (flags & SIMDURL_REJECT_NUL)) ||
       (c < 32 && (flags & SIMDURL_REJECT_CONTROL))) {
      result.status = SIMDURL_REJECTED;
      result.written = 0;
      return result;
    }
    output[result.written++] = (char)c;
  }
  return result;
}

static void check_encode_case(encode_backend backend, const char *input,
                              size_t length, unsigned int flags,
                              size_t source, size_t destination)
{
  size_t capacity = length * 3, remaining = length, consumed, expected;
  size_t source_prefix, output_prefix;
  size_t padding = GUARD_SIZE * 2 + ALIGNMENTS * 2;
  char *source_storage = (char *)malloc(length + padding);
  char *storage = (char *)malloc(capacity + padding);
  char *reference = (char *)malloc(capacity + 1);
  char *output;
  const char *next;
  record_case(input, length, flags, source, destination, 0);
  CHECK(source_storage != NULL && storage != NULL && reference != NULL);
  source_prefix = aligned_prefix(source_storage, source);
  output_prefix = aligned_prefix(storage, destination);
  memset(source_storage, 0x5a, length + source_prefix + GUARD_SIZE);
  memcpy(source_storage + source_prefix, input, length);
  memset(storage, 0x5a, capacity + output_prefix + GUARD_SIZE);
  output = storage + output_prefix;
  next = source_storage + source_prefix;

  if(backend) {
    backend(&next, &remaining, &output, flags);
    if(length >= 32)
      ++kernel_calls;
  }
  else {
    simdurl_result result = simdurl_detail_encode_tail(next, remaining, output,
                                                      capacity, flags, 0, 0);
    CHECK(result.status == SIMDURL_OK);
    output += result.written;
    next += remaining;
    remaining = 0;
    ++kernel_calls;
  }

  CHECK(remaining <= length);
  consumed = length - remaining;
  CHECK(next == source_storage + source_prefix + consumed);
  CHECK(remaining < 32);
  expected = reference_encode(input, consumed, reference, flags);
  CHECK(output == storage + output_prefix + expected);
  CHECK(memcmp(storage + output_prefix, reference, expected) == 0);
  CHECK(memcmp(source_storage + source_prefix, input, length) == 0);
  check_canaries(source_storage, source_prefix, length);
  check_canaries(storage, output_prefix, capacity);

  free(reference);
  free(storage);
  free(source_storage);
}

static simdurl_result direct_decode(decode_backend backend, const char *input,
                                    size_t length, char *output,
                                    unsigned int flags, int in_place)
{
  const char *next = input;
  char *destination = output;
  size_t remaining = length, written;
  unsigned char reject_limit = (flags & SIMDURL_REJECT_CONTROL) ? 32 :
                               (flags & SIMDURL_REJECT_NUL) ? 1 : 0;
  if(backend && length >= 32) {
    ++kernel_calls;
    if(!backend(&next, &remaining, &destination, reject_limit, flags, in_place)) {
      simdurl_result rejected = { SIMDURL_REJECTED, 0 };
      return rejected;
    }
    CHECK(remaining < 32);
    CHECK(next == input + length - remaining);
    CHECK(destination >= output && destination <= output + length - remaining);
  }
  if(!backend)
    ++kernel_calls;
  written = (size_t)(destination - output);
  return simdurl_detail_decode_tail(next, remaining, destination,
                                    length - written, flags & SIMDURL_FORM,
                                    reject_limit, written);
}

static void check_decode_case(decode_backend backend, const char *input,
                              size_t length, unsigned int flags,
                              size_t source, size_t destination, int in_place)
{
  size_t source_prefix, output_prefix;
  size_t padding = GUARD_SIZE * 2 + ALIGNMENTS * 2;
  char *source_storage = (char *)malloc(length + padding);
  char *storage = in_place ? source_storage : (char *)malloc(length + padding);
  char *reference = (char *)malloc(length + 1);
  simdurl_result result, expected;
  record_case(input, length, flags, source, in_place ? source : destination,
               in_place);
  CHECK(source_storage != NULL && storage != NULL && reference != NULL);
  source_prefix = aligned_prefix(source_storage, source);
  output_prefix = in_place ? source_prefix : aligned_prefix(storage, destination);
  memset(source_storage, 0x5a, length + source_prefix + GUARD_SIZE);
  if(!in_place)
    memset(storage, 0x5a, length + output_prefix + GUARD_SIZE);
  memcpy(source_storage + source_prefix, input, length);
  expected = reference_decode(input, length, reference, flags);
  result = direct_decode(backend, source_storage + source_prefix, length,
                          storage + output_prefix, flags, in_place);
  CHECK(result.status == expected.status);
  CHECK(result.written == expected.written);
  if(result.status == SIMDURL_OK)
    CHECK(memcmp(storage + output_prefix, reference, result.written) == 0);
  if(!in_place)
    CHECK(memcmp(source_storage + source_prefix, input, length) == 0);
  check_canaries(source_storage, source_prefix, length);
  check_canaries(storage, output_prefix, length);
  free(reference);
  if(!in_place)
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

static void test_encoder(encode_backend backend, const char *name)
{
  static const char literal[] = "aZ09-._";
  static const char mixed[] = "aZ09-._~* +%/\x80\xff";
  static const size_t boundaries[] = { 31, 32, 33, 63, 64, 65, 127, 128, 129 };
  char input[SIMDURL_TEST_MAX_INPUT];
  uint32_t seed = UINT32_C(0x35dabc97);
  unsigned int flags, byte, position, trial;
  size_t length, i, source, destination, boundary;
  backend_name = name;
  cases = kernel_calls = 0;

  for(flags = SIMDURL_URI; flags <= SIMDURL_FORM; ++flags) {
    context = "encode every byte and short length";
    for(byte = 0; byte < 256; ++byte) {
      memset(input, (int)byte, sizeof(input));
      for(length = 0; length <= 128; ++length)
        check_encode_case(backend, input, length, flags, length % ALIGNMENTS,
                            byte % ALIGNMENTS);
    }
    context = "encode selected byte in every SIMD lane";
    for(byte = 0; byte < 256; ++byte) {
      for(position = 0; position < 64; ++position) {
        memset(input, 'a', sizeof(input));
        input[position] = (char)byte;
        check_encode_case(backend, input, 128, flags, position,
                            (position * 13) % ALIGNMENTS);
      }
    }
    context = "encode independent source and output alignments";
    for(boundary = 0; boundary < sizeof(boundaries) / sizeof(boundaries[0]);
        ++boundary) {
      length = boundaries[boundary];
      for(i = 0; i < length; ++i)
        input[i] = mixed[i % (sizeof(mixed) - 1)];
      for(source = 0; source < ALIGNMENTS; ++source)
        for(destination = 0; destination < ALIGNMENTS; ++destination)
          check_encode_case(backend, input, length, flags, source, destination);
    }
    context = "encode literal and mixed runs";
    for(length = 0; length <= sizeof(input); length += 17) {
      for(i = 0; i < length; ++i)
        input[i] = literal[i % (sizeof(literal) - 1)];
      check_encode_case(backend, input, length, flags, length % ALIGNMENTS, 0);
      for(i = 0; i < length; ++i)
        input[i] = mixed[i % (sizeof(mixed) - 1)];
      check_encode_case(backend, input, length, flags, 0, length % ALIGNMENTS);
    }
    context = "encode deterministic random seed 35dabc97";
    for(trial = 0; trial < 2000; ++trial) {
      length = random_word(&seed) % (sizeof(input) + 1);
      for(i = 0; i < length; ++i)
        input[i] = (char)random_word(&seed);
      check_encode_case(backend, input, length, flags,
                          random_word(&seed) % ALIGNMENTS,
                          random_word(&seed) % ALIGNMENTS);
    }
  }
  printf("SIMDURL_BACKEND operation=encode backend=%s compiled=1 executed=1 "
         "skipped=0 cases=%lu kernel_calls=%lu\n", name, cases, kernel_calls);
}

static void test_decoder(decode_backend backend, const char *name)
{
  static const char *const tokens[] = {
    "%00", "%01", "%1f", "%20", "%2B", "%7F", "%80", "%fF", "%G0",
    "%0g", "%%%", "%", "%A", "+", "\0", "\x1f", "\x7f", "\xff"
  };
  static const size_t token_lengths[] = {
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 1, 1, 1, 1, 1
  };
  static const char mixed[] = "%41+%2B%00%1f%20%aF%FF%g0%0G%%a";
  static const char safe_mixed[] = "%41+%2B%20%aF%FF%g0%0G%%a";
  static const size_t boundaries[] = { 31, 32, 33, 63, 64, 65, 127, 128, 129 };
  static const char alphabet[] = "0123456789ABCDEF";
  char input[SIMDURL_TEST_MAX_INPUT];
  uint32_t seed = UINT32_C(0x37d98a45);
  unsigned int flags, byte, trial;
  size_t length, position, token, i, source, destination, boundary;
  unsigned int high, low;
  int in_place;
  backend_name = name;
  cases = kernel_calls = 0;

  for(flags = 0; flags < 8; ++flags) {
    context = "decode valid, malformed and truncated escapes in every lane";
    for(position = 0; position < 160; ++position) {
      for(token = 0; token < sizeof(tokens) / sizeof(tokens[0]); ++token) {
        memset(input, 'a', sizeof(input));
        memcpy(input + position, tokens[token], token_lengths[token]);
        for(in_place = 0; in_place < 2; ++in_place) {
          check_decode_case(backend, input, 193, flags, position % ALIGNMENTS,
                              (position * 13) % ALIGNMENTS, in_place);
          check_decode_case(backend, input, position + token_lengths[token],
                              flags, position % ALIGNMENTS, 0, in_place);
        }
      }
    }
    context = "decode every raw and escaped byte in every SIMD lane";
    for(byte = 0; byte < 256; ++byte) {
      for(position = 0; position < 32; ++position) {
        memset(input, 'a', 96);
        input[position] = (char)byte;
        for(in_place = 0; in_place < 2; ++in_place)
          check_decode_case(backend, input, 96, flags, position,
                              byte % ALIGNMENTS, in_place);
        input[position] = '%';
        input[position + 1] = alphabet[byte >> 4];
        input[position + 2] = alphabet[byte & 15];
        for(in_place = 0; in_place < 2; ++in_place)
          check_decode_case(backend, input, 96, flags, position,
                              byte % ALIGNMENTS, in_place);
      }
    }
    context = "decode independent source and output alignments";
    for(boundary = 0; boundary < sizeof(boundaries) / sizeof(boundaries[0]);
        ++boundary) {
      length = boundaries[boundary];
      for(i = 0; i < length; ++i)
        input[i] = safe_mixed[i % (sizeof(safe_mixed) - 1)];
      for(source = 0; source < ALIGNMENTS; ++source) {
        for(destination = 0; destination < ALIGNMENTS; ++destination)
          check_decode_case(backend, input, length, flags, source, destination, 0);
        check_decode_case(backend, input, length, flags, source, source, 1);
      }
    }
    context = "decode contraction, long literal copy and vector tails";
    for(length = 0; length <= sizeof(input); length += length < 256 ? 1 : 17) {
      for(i = 0; i < length; ++i)
        input[i] = mixed[i % (sizeof(mixed) - 1)];
      for(in_place = 0; in_place < 2; ++in_place)
        check_decode_case(backend, input, length, flags, length % ALIGNMENTS,
                            (length * 7) % ALIGNMENTS, in_place);
      /* A contraction before a long literal run exercises overlapping memmove. */
      memset(input, 'a', length);
      if(length >= 3)
        memcpy(input, "%41", 3);
      if(length >= 6)
        memcpy(input + length - 3, "%2B", 3);
      for(in_place = 0; in_place < 2; ++in_place)
        check_decode_case(backend, input, length, flags, length % ALIGNMENTS,
                            (length * 7) % ALIGNMENTS, in_place);
    }
    context = "decode deterministic random seed 37d98a45";
    for(trial = 0; trial < 500; ++trial) {
      length = random_word(&seed) % (sizeof(input) + 1);
      for(i = 0; i < length; ++i) {
        unsigned int value = random_word(&seed);
        input[i] = trial % 2 ? safe_mixed[value % (sizeof(safe_mixed) - 1)] :
                             (char)value;
      }
      for(in_place = 0; in_place < 2; ++in_place)
        check_decode_case(backend, input, length, flags,
                            random_word(&seed) % ALIGNMENTS,
                            random_word(&seed) % ALIGNMENTS, in_place);
    }
  }
  /* Every pair following '%' also reaches the SIMD classifier, including
   * the 16-byte shuffle boundary and both deferred 32-byte block positions.
   * Other tests cover all flag combinations; these two modes cover both
   * unrestricted bytes and the strongest rejection policy with form rules. */
  context = "decode every escape byte pair at shuffle and block boundaries";
  for(high = 0; high < 256; ++high) {
    for(low = 0; low < 256; ++low) {
      for(position = 15; position <= 31; position += position == 15 ? 15 : 1) {
        memset(input, 'a', 96);
        input[position] = '%';
        input[position + 1] = (char)high;
        input[position + 2] = (char)low;
        for(flags = 0; flags <= 7; flags += 7) {
          for(in_place = 0; in_place < 2; ++in_place)
            check_decode_case(backend, input, 96, flags, high % ALIGNMENTS,
                                low % ALIGNMENTS, in_place);
        }
      }
    }
  }
  printf("SIMDURL_BACKEND operation=decode backend=%s compiled=1 executed=1 "
         "skipped=0 cases=%lu kernel_calls=%lu\n", name, cases, kernel_calls);
}

/* No padding: sanitizers can detect overreads or stores hidden by canaries.
 * SIMD codec preconditions require worst-case capacity, even if the exact
 * decoded/encoded result is shorter. */
static void test_exact_allocations(encode_backend encode, decode_backend decode,
                                   const char *name)
{
  size_t length, i, remaining;
  unsigned int flags;
  static const char pattern[] = "%41+%2B%20%%%G0";
  backend_name = name;
  context = "exact source and worst-case output allocations";
  for(length = 1; length <= 1024; ++length) {
    char *input = (char *)malloc(length);
    char *output = (char *)malloc(length * 3);
    char *decoded = (char *)malloc(length);
    char *reference = (char *)malloc(length * 3);
    /* Record only after allocating; no failure reads an uninitialized span. */
    case_length = 0;
    CHECK(input != NULL && output != NULL && decoded != NULL && reference != NULL);
    for(i = 0; i < length; ++i)
      input[i] = pattern[i % (sizeof(pattern) - 1)];
    for(flags = 0; flags < 8; ++flags) {
      simdurl_result expected, result;
      record_case(input, length, flags, 0, 0, 0);
      expected = reference_decode(input, length, reference, flags);
      result = direct_decode(decode, input, length, decoded, flags, 0);
      CHECK(result.status == expected.status && result.written == expected.written);
      CHECK(memcmp(decoded, reference, result.written) == 0);
      memcpy(decoded, input, length);
      record_case(input, length, flags, 0, 0, 1);
      result = direct_decode(decode, decoded, length, decoded, flags, 1);
      CHECK(result.status == expected.status && result.written == expected.written);
      CHECK(memcmp(decoded, reference, result.written) == 0);
      if(flags < 2) {
        const char *next = input;
        char *destination = output;
        remaining = length;
        if(encode) {
          encode(&next, &remaining, &destination, flags);
          CHECK(remaining < 32 && remaining <= length);
          CHECK(next == input + length - remaining);
          i = reference_encode(input, length - remaining, reference, flags);
          CHECK(destination == output + i);
          CHECK(memcmp(output, reference, i) == 0);
        }
        else {
          result = simdurl_detail_encode_tail(input, length, output, length * 3,
                                               flags, 0, 0);
          i = reference_encode(input, length, reference, flags);
          CHECK(result.status == SIMDURL_OK && result.written == i);
          CHECK(memcmp(output, reference, i) == 0);
        }
      }
    }
    free(reference);
    free(decoded);
    free(output);
    free(input);
  }
}

static void report_skipped(const char *operation, const char *name, int compiled)
{
  printf("SIMDURL_BACKEND operation=%s backend=%s compiled=%d executed=0 "
         "skipped=1 cases=0 kernel_calls=0 reason=%s\n", operation, name,
         compiled, compiled ? "cpu_unsupported" : "not_compiled");
}

int main(int argc, char **argv)
{
  unsigned int required = 0, available = 1;
  int arg, compiled = 0, avx2 = 0, vbmi2 = 0;
  for(arg = 1; arg < argc; ++arg) {
    if(strcmp(argv[arg], "--require=portable") == 0)
      required |= 1;
    else if(strcmp(argv[arg], "--require=avx2") == 0)
      required |= 2;
    else if(strcmp(argv[arg], "--require=vbmi2") == 0)
      required |= 4;
    else {
      fprintf(stderr, "Usage: %s [--require=portable] [--require=avx2] "
              "[--require=vbmi2]\n", argv[0]);
      return EXIT_FAILURE;
    }
  }
#ifdef SIMDURL_DETAIL_X86
  compiled = 1;
  avx2 = simdurl_detail_has_avx2();
  vbmi2 = simdurl_detail_has_vbmi2();
#endif
  test_exact_allocations(NULL, NULL, "portable");
  test_encoder(NULL, "portable");
  test_decoder(NULL, "portable");
#ifdef SIMDURL_DETAIL_X86
  if(avx2) {
    available |= 2;
    test_exact_allocations(simdurl_detail_encode_avx2, NULL, "avx2");
    test_encoder(simdurl_detail_encode_avx2, "avx2");
  }
  if(vbmi2) {
    available |= 4;
    test_exact_allocations(simdurl_detail_encode_vbmi2,
                             simdurl_detail_decode_vbmi2, "vbmi2");
    test_encoder(simdurl_detail_encode_vbmi2, "vbmi2");
    test_decoder(simdurl_detail_decode_vbmi2, "vbmi2");
  }
#endif
  if(!avx2)
    report_skipped("encode", "avx2", compiled);
  if(!vbmi2) {
    report_skipped("encode", "vbmi2", compiled);
    report_skipped("decode", "vbmi2", compiled);
  }
  if(required & ~available) {
    fprintf(stderr, "Required codec backend did not execute:");
    if((required & 2) && !(available & 2))
      fputs(" avx2", stderr);
    if((required & 4) && !(available & 4))
      fputs(" vbmi2", stderr);
    fputc('\n', stderr);
    return EXIT_FAILURE;
  }
  printf("%lu direct backend checks passed\n", checks);
  return EXIT_SUCCESS;
}
