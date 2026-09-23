/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_HEX_H
#define SIMDURL_DETAIL_HEX_H

/* Make the API's non-overlap contract visible to the optimizer. This helps
 * fixed-size portable calls vectorize instead of unrolling into scalar code. */
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#define SIMDURL_DETAIL_HEX_RESTRICT __restrict
#else
#define SIMDURL_DETAIL_HEX_RESTRICT
#endif

/* Arithmetic conversion avoids data-dependent table reads and lets compilers
 * vectorize this loop when their target and cost model make that useful. */
static inline void simdurl_detail_hex_encode_portable(
  const char *SIMDURL_DETAIL_HEX_RESTRICT input, size_t length,
  char *SIMDURL_DETAIL_HEX_RESTRICT output, unsigned int flags)
{
  unsigned int letter_offset = (flags & SIMDURL_HEX_UPPER) ? 7 : 39;
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned int byte = (unsigned char)input[i];
    unsigned int high = byte >> 4;
    unsigned int low = byte & 15;
    output[2 * i] = (char)('0' + high + (high > 9 ? letter_offset : 0));
    output[2 * i + 1] = (char)('0' + low + (low > 9 ? letter_offset : 0));
  }
}

#ifdef SIMDURL_DETAIL_X86
/* Small scalar tail keeps bounded SIMD kernels compact. */
static inline void simdurl_detail_hex_encode_tail(
  const char *input, size_t length, char *output, unsigned int flags)
{
  const char *digits = (flags & SIMDURL_HEX_UPPER) ?
    "0123456789ABCDEF" : "0123456789abcdef";
  size_t i;
  for(i = 0; i < length; ++i) {
    unsigned int byte = (unsigned char)input[i];
    output[2 * i] = digits[byte >> 4];
    output[2 * i + 1] = digits[byte & 15];
  }
}

/* Keep vector copies below fixed-size so they compile to vector stores. */
static inline void simdurl_detail_hex_encode_sse2(
  const char *input, size_t length, char *output, unsigned int flags)
{
  const __m128i nibble_mask = _mm_set1_epi8(15);
  const __m128i nine = _mm_set1_epi8(9);
  const __m128i zero = _mm_set1_epi8('0');
  const __m128i letter_offset = _mm_set1_epi8(
    (flags & SIMDURL_HEX_UPPER) ? 7 : 39);
  while(length >= 16) {
    __m128i bytes, high, low, first, second;
    memcpy(&bytes, input, sizeof(bytes));
    high = _mm_and_si128(_mm_srli_epi16(bytes, 4), nibble_mask);
    low = _mm_and_si128(bytes, nibble_mask);
    high = _mm_add_epi8(_mm_add_epi8(high, zero),
      _mm_and_si128(_mm_cmpgt_epi8(high, nine), letter_offset));
    low = _mm_add_epi8(_mm_add_epi8(low, zero),
      _mm_and_si128(_mm_cmpgt_epi8(low, nine), letter_offset));
    first = _mm_unpacklo_epi8(high, low);
    second = _mm_unpackhi_epi8(high, low);
    memcpy(output, &first, sizeof(first));
    memcpy(output + 16, &second, sizeof(second));
    input += 16;
    output += 32;
    length -= 16;
  }
  /* An eight-byte input remainder still fills a complete output vector. */
  if(length >= 8) {
    __m128i bytes = _mm_setzero_si128();
    __m128i high, low, result;
    memcpy(&bytes, input, 8);
    high = _mm_and_si128(_mm_srli_epi16(bytes, 4), nibble_mask);
    low = _mm_and_si128(bytes, nibble_mask);
    high = _mm_add_epi8(_mm_add_epi8(high, zero),
      _mm_and_si128(_mm_cmpgt_epi8(high, nine), letter_offset));
    low = _mm_add_epi8(_mm_add_epi8(low, zero),
      _mm_and_si128(_mm_cmpgt_epi8(low, nine), letter_offset));
    result = _mm_unpacklo_epi8(high, low);
    memcpy(output, &result, sizeof(result));
    input += 8;
    output += 16;
    length -= 8;
  }
  if(length)
    simdurl_detail_hex_encode_tail(input, length, output, flags);
}

SIMDURL_DETAIL_TARGET_AVX2
static inline void simdurl_detail_hex_encode_avx2(
  const char *input, size_t length, char *output, unsigned int flags)
{
  const char *alphabet = (flags & SIMDURL_HEX_UPPER) ?
    "0123456789ABCDEF" : "0123456789abcdef";
  const __m256i nibble_mask = _mm256_set1_epi8(15);
  __m128i table128;
  __m256i table;
  memcpy(&table128, alphabet, sizeof(table128));
  table = _mm256_broadcastsi128_si256(table128);
  while(length >= 32) {
    __m256i bytes, high, low, even, odd, first, second;
    memcpy(&bytes, input, sizeof(bytes));
    high = _mm256_and_si256(_mm256_srli_epi16(bytes, 4), nibble_mask);
    low = _mm256_and_si256(bytes, nibble_mask);
    high = _mm256_shuffle_epi8(table, high);
    low = _mm256_shuffle_epi8(table, low);
    even = _mm256_unpacklo_epi8(high, low);
    odd = _mm256_unpackhi_epi8(high, low);
    /* Unpacking operates within each 128-bit lane. Join the two halves of
     * the first sixteen input bytes before those of the next sixteen. */
    first = _mm256_permute2x128_si256(even, odd, 0x20);
    second = _mm256_permute2x128_si256(even, odd, 0x31);
    memcpy(output, &first, sizeof(first));
    memcpy(output + 32, &second, sizeof(second));
    input += 32;
    output += 64;
    length -= 32;
  }
  if(length)
    simdurl_detail_hex_encode_sse2(input, length, output, flags);
}
#endif

#undef SIMDURL_DETAIL_HEX_RESTRICT
#endif
