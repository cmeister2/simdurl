/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_SCAN_H
#define SIMDURL_DETAIL_SCAN_H

SIMDURL_DETAIL_INLINE int simdurl_detail_scan_tail(
  const char *input, size_t length, unsigned int checks)
{
  while(length) {
    unsigned char byte = (unsigned char)*input++;
    if(((checks & SIMDURL_CHECK_C0) && byte < 32) ||
       ((checks & SIMDURL_CHECK_DEL) && byte == 127) ||
       ((checks & SIMDURL_CHECK_SPACE) && byte == 32))
      return 1;
    --length;
  }
  return 0;
}

/* Fixed-size reductions allow compiler vectorization without reading past
 * the span. Check each block before advancing so early rejection stays cheap.
 */
static inline int simdurl_detail_scan_portable(
  const char *input, size_t length, unsigned int checks)
{
  if(!checks)
    return 0;
  while(length >= 32) {
    unsigned char minimum = 255;
    unsigned char has_del = 0;
    unsigned char has_space = 0;
    size_t i;
    for(i = 0; i < 32; ++i) {
      unsigned char byte = (unsigned char)input[i];
      minimum = byte < minimum ? byte : minimum;
      has_del |= (unsigned char)(byte == 127);
      has_space |= (unsigned char)(byte == 32);
    }
    if(((checks & SIMDURL_CHECK_C0) && minimum < 32) ||
       ((checks & SIMDURL_CHECK_DEL) && has_del) ||
       ((checks & SIMDURL_CHECK_SPACE) && has_space))
      return 1;
    input += 32;
    length -= 32;
  }
  return simdurl_detail_scan_tail(input, length, checks);
}

#ifdef SIMDURL_DETAIL_X86
/* SSE2 is part of the x86-64 baseline. Masks keep disabled checks from
 * rejecting any byte, including NUL; unsigned comparisons allow high bytes.
 */
static inline int simdurl_detail_scan_sse2(
  const char *input, size_t length, unsigned int checks)
{
  const __m128i lower = _mm_set1_epi8((checks & SIMDURL_CHECK_C0) ? 32 : 0);
  const __m128i del = _mm_set1_epi8(127);
  const __m128i space = _mm_set1_epi8(32);
  const __m128i del_mask = _mm_set1_epi8(
    (checks & SIMDURL_CHECK_DEL) ? -1 : 0);
  const __m128i space_mask = _mm_set1_epi8(
    (checks & SIMDURL_CHECK_SPACE) ? -1 : 0);
  if(!checks)
    return 0;
  while(length >= 16) {
    __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)input);
    __m128i in_range = _mm_cmpeq_epi8(_mm_max_epu8(bytes, lower), bytes);
    __m128i matches = _mm_or_si128(
      _mm_and_si128(_mm_cmpeq_epi8(bytes, del), del_mask),
      _mm_and_si128(_mm_cmpeq_epi8(bytes, space), space_mask));
    if(_mm_movemask_epi8(_mm_andnot_si128(matches, in_range)) != 0xffff)
      return 1;
    input += 16;
    length -= 16;
  }
  return simdurl_detail_scan_tail(input, length, checks);
}

SIMDURL_DETAIL_TARGET_AVX2
static inline int simdurl_detail_scan_avx2(
  const char *input, size_t length, unsigned int checks)
{
  const __m256i lower = _mm256_set1_epi8((checks & SIMDURL_CHECK_C0) ? 32 : 0);
  const __m256i del = _mm256_set1_epi8(127);
  const __m256i space = _mm256_set1_epi8(32);
  const __m256i del_mask = _mm256_set1_epi8(
    (checks & SIMDURL_CHECK_DEL) ? -1 : 0);
  const __m256i space_mask = _mm256_set1_epi8(
    (checks & SIMDURL_CHECK_SPACE) ? -1 : 0);
  if(!checks)
    return 0;
  while(length >= 32) {
    __m256i bytes = _mm256_loadu_si256((const __m256i *)(const void *)input);
    __m256i in_range = _mm256_cmpeq_epi8(_mm256_max_epu8(bytes, lower), bytes);
    __m256i matches = _mm256_or_si256(
      _mm256_and_si256(_mm256_cmpeq_epi8(bytes, del), del_mask),
      _mm256_and_si256(_mm256_cmpeq_epi8(bytes, space), space_mask));
    if(_mm256_movemask_epi8(_mm256_andnot_si256(matches, in_range)) != -1)
      return 1;
    input += 32;
    length -= 32;
  }
  return simdurl_detail_scan_sse2(input, length, checks);
}
#endif
#endif
