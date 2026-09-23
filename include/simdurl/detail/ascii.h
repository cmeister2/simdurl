/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_ASCII_H
#define SIMDURL_DETAIL_ASCII_H

/* Do not restrict the pointers: exact in-place conversion is supported.
 * Keep the comparison byte-sized so compiler vectorization does not widen
 * each input byte to a 32-bit lane. This also leaves short tails inexpensive.
 */
SIMDURL_DETAIL_INLINE void simdurl_detail_ascii_lower_portable(
  const char *input, size_t length, char *output)
{
  const unsigned char *source = (const unsigned char *)(const void *)input;
  unsigned char *destination = (unsigned char *)(void *)output;
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned char byte = source[i];
    destination[i] = (unsigned char)((byte >= 0x41 && byte <= 0x5a) ?
                                      byte + 0x20 : byte);
  }
}

#ifdef SIMDURL_DETAIL_X86
/* Both signed comparisons must match, so bytes above 127 remain unchanged. */
static inline void simdurl_detail_ascii_lower_sse2(
  const char *input, size_t length, char *output)
{
  const __m128i before_uppercase = _mm_set1_epi8(0x40);
  const __m128i after_uppercase = _mm_set1_epi8(0x5b);
  const __m128i lowercase_bit = _mm_set1_epi8(0x20);
  while(length >= 16) {
    __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)input);
    __m128i uppercase = _mm_and_si128(
      _mm_cmpgt_epi8(bytes, before_uppercase),
      _mm_cmpgt_epi8(after_uppercase, bytes));
    bytes = _mm_or_si128(bytes, _mm_and_si128(uppercase, lowercase_bit));
    _mm_storeu_si128((__m128i *)(void *)output, bytes);
    input += 16;
    output += 16;
    length -= 16;
  }
  simdurl_detail_ascii_lower_portable(input, length, output);
}

SIMDURL_DETAIL_TARGET_AVX2
static inline void simdurl_detail_ascii_lower_avx2(
  const char *input, size_t length, char *output)
{
  const __m256i before_uppercase = _mm256_set1_epi8(0x40);
  const __m256i after_uppercase = _mm256_set1_epi8(0x5b);
  const __m256i lowercase_bit = _mm256_set1_epi8(0x20);
  while(length >= 32) {
    __m256i bytes = _mm256_loadu_si256((const __m256i *)(const void *)input);
    __m256i uppercase = _mm256_and_si256(
      _mm256_cmpgt_epi8(bytes, before_uppercase),
      _mm256_cmpgt_epi8(after_uppercase, bytes));
    bytes = _mm256_or_si256(
      bytes, _mm256_and_si256(uppercase, lowercase_bit));
    _mm256_storeu_si256((__m256i *)(void *)output, bytes);
    input += 32;
    output += 32;
    length -= 32;
  }
  simdurl_detail_ascii_lower_sse2(input, length, output);
}
#endif
#endif
