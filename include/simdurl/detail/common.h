/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_COMMON_H
#define SIMDURL_DETAIL_COMMON_H

#include <string.h>

#if defined(_MSC_VER)
#define SIMDURL_DETAIL_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define SIMDURL_DETAIL_INLINE static __inline__ __attribute__((always_inline))
#else
#define SIMDURL_DETAIL_INLINE static inline
#endif

/* clang-cl lacks the runtime behind __builtin_cpu_supports. Keep the portable
 * backend there, on MSVC, on older compilers, and on non-x86 architectures. */
#if !defined(SIMDURL_DISABLE_SIMD) && defined(__x86_64__) && \
    !defined(_MSC_VER) && !defined(__INTEL_COMPILER) && \
    ((defined(__clang__) && __clang_major__ >= 10) || \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 9))
#define SIMDURL_DETAIL_X86 1
#include <immintrin.h>
#define SIMDURL_DETAIL_TARGET_AVX2 __attribute__((target("avx2")))
#define SIMDURL_DETAIL_TARGET_VBMI2 \
  __attribute__((target("avx512vbmi2,avx512vl,avx512bw,avx2,popcnt")))

static inline int simdurl_detail_has_avx2(void)
{
#if defined(__AVX2__)
  return 1;
#else
  return __builtin_cpu_supports("avx2") != 0;
#endif
}

static inline int simdurl_detail_has_vbmi2(void)
{
#if defined(__AVX512VBMI2__) && defined(__AVX512VL__) && \
    defined(__AVX512BW__) && defined(__AVX2__) && defined(__POPCNT__)
  return 1;
#else
  return __builtin_cpu_supports("avx512vbmi2") &&
         __builtin_cpu_supports("avx512vl") &&
         __builtin_cpu_supports("avx512bw") &&
         __builtin_cpu_supports("avx2") && __builtin_cpu_supports("popcnt");
#endif
}
#endif

SIMDURL_DETAIL_INLINE int simdurl_detail_unreserved(unsigned char c,
                                                   unsigned int flags)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
         c == ((flags & SIMDURL_FORM) ? '*' : '~');
}

SIMDURL_DETAIL_INLINE unsigned int simdurl_detail_hex(unsigned char c)
{
  /* Invalid bytes map to 16, so (high | low) < 16 validates both digits.
   * A bounded byte lookup avoids data-dependent branches for each hex digit. */
  static const unsigned char values[256] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 16, 16, 16, 16, 16, 16,
    16, 10, 11, 12, 13, 14, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 10, 11, 12, 13, 14, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
  };
  return values[c];
}

/* Read both ends before storing: an in-place decoded span can overlap its
 * destination. Fixed-width copies cover only bytes within the literal span. */
SIMDURL_DETAIL_INLINE void simdurl_detail_copy_literals(char *output,
                                                       const char *input,
                                                       size_t length)
{
  if(length == 1)
    *output = *input;
  else if(length > 16)
    memmove(output, input, length);
  else if(length >= 8) {
    uint64_t first, last;
    memcpy(&first, input, sizeof(first));
    memcpy(&last, input + length - sizeof(last), sizeof(last));
    memcpy(output, &first, sizeof(first));
    memcpy(output + length - sizeof(last), &last, sizeof(last));
  }
  else if(length >= 4) {
    uint32_t first, last;
    memcpy(&first, input, sizeof(first));
    memcpy(&last, input + length - sizeof(last), sizeof(last));
    memcpy(output, &first, sizeof(first));
    memcpy(output + length - sizeof(last), &last, sizeof(last));
  }
  else if(length >= 2) {
    uint16_t first, last;
    memcpy(&first, input, sizeof(first));
    memcpy(&last, input + length - sizeof(last), sizeof(last));
    memcpy(output, &first, sizeof(first));
    memcpy(output + length - sizeof(last), &last, sizeof(last));
  }
}

/* Initialize *next_percent to the span start, then reuse it only while
 * consuming successive literal runs from that span. Cache the next percent
 * marker (or the span end) so each percent search covers new input. Plus
 * searches stop at their first match; total search work is linear in the span.
 */
SIMDURL_DETAIL_INLINE size_t simdurl_detail_literal_length(
  const char *input, size_t length, unsigned int form, const char **next_percent)
{
  const char *marker;
  if(!length)
    return 0;
  if(!form) {
    marker = (const char *)memchr(input, '%', length);
    return marker ? (size_t)(marker - input) : length;
  }
  marker = *next_percent;
  if(input >= marker) {
    marker = (const char *)memchr(input, '%', length);
    if(!marker)
      marker = input + length;
    *next_percent = marker;
  }
  length = (size_t)(marker - input);
  marker = (const char *)memchr(input, '+', length);
  return marker ? (size_t)(marker - input) : length;
}

/* A minimum reduction validates the entire literal span and allows the
 * compiler to vectorize control-byte checks without a branch per byte. */
SIMDURL_DETAIL_INLINE int simdurl_detail_literals_allowed(
  const char *input, size_t length, unsigned char minimum_allowed)
{
  unsigned char minimum = 255;
  size_t offset;
  if(!minimum_allowed)
    return 1;
  if(minimum_allowed == 1)
    return memchr(input, 0, length) == NULL;
  for(offset = 0; offset < length; ++offset) {
    unsigned char byte = (unsigned char)input[offset];
    minimum = byte < minimum ? byte : minimum;
  }
  return minimum >= minimum_allowed;
}

static inline simdurl_result simdurl_detail_result(simdurl_status status,
                                                  size_t written)
{
  simdurl_result r;
  r.status = status;
  r.written = written;
  return r;
}
#endif
