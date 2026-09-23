/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_ENCODE_H
#define SIMDURL_DETAIL_ENCODE_H

#ifdef SIMDURL_DETAIL_X86

/* Specializing the mode at the two callers below keeps form handling out of
 * the URI loop. No helper returns a vector, avoiding vector ABI differences
 * between baseline and target-attributed functions on Windows toolchains. */
SIMDURL_DETAIL_TARGET_VBMI2
SIMDURL_DETAIL_INLINE void simdurl_detail_encode_vbmi2_mode(
  const char **input, size_t *remaining, char **output, unsigned int flags)
{
  const char *src = *input;
  size_t n = *remaining;
  char *dst = *output;

  while(n >= 32) {
    __m128i bytes;
    __mmask16 raw, spaces = 0;
    memcpy(&bytes, src, sizeof(bytes));
    raw = _mm_cmp_epu8_mask(
      _mm_sub_epi8(_mm_or_si128(bytes, _mm_set1_epi8(32)),
                   _mm_set1_epi8('a')),
      _mm_set1_epi8(25), _MM_CMPINT_LE);
    raw |= _mm_cmp_epu8_mask(_mm_sub_epi8(bytes, _mm_set1_epi8('0')),
                            _mm_set1_epi8(9), _MM_CMPINT_LE);
    raw |= _mm_cmpeq_epi8_mask(bytes, _mm_set1_epi8('-'));
    raw |= _mm_cmpeq_epi8_mask(bytes, _mm_set1_epi8('.'));
    raw |= _mm_cmpeq_epi8_mask(bytes, _mm_set1_epi8('_'));
    raw |= _mm_cmpeq_epi8_mask(bytes, _mm_set1_epi8(
      (flags & SIMDURL_FORM) ? '*' : '~'));
    if(flags & SIMDURL_FORM) {
      spaces = _mm_cmpeq_epi8_mask(bytes, _mm_set1_epi8(' '));
      raw |= spaces;
    }

    if(raw == (__mmask16)0xffffU) {
      if(flags & SIMDURL_FORM)
        bytes = _mm_mask_mov_epi8(bytes, spaces, _mm_set1_epi8('+'));
      memcpy(dst, &bytes, sizeof(bytes));
      dst += 16;
    }
    else {
      __mmask16 escape = (__mmask16)~raw;
      __m512i lanes = _mm512_cvtepu8_epi32(bytes);
      __m512i high = _mm512_srli_epi32(lanes, 4);
      __m512i low = _mm512_and_si512(lanes, _mm512_set1_epi32(15));
      __m512i first, candidate, packed;
      __mmask64 keep;

      high = _mm512_add_epi32(high, _mm512_set1_epi32('0'));
      low = _mm512_add_epi32(low, _mm512_set1_epi32('0'));
      high = _mm512_mask_add_epi32(high,
        _mm512_cmpgt_epi32_mask(high, _mm512_set1_epi32('9')),
        high, _mm512_set1_epi32(7));
      low = _mm512_mask_add_epi32(low,
        _mm512_cmpgt_epi32_mask(low, _mm512_set1_epi32('9')),
        low, _mm512_set1_epi32(7));

      /* Widening creates sixteen independent four-byte lanes. Each contains
       * either [raw, hex, hex, 0] or ['%', hex, hex, 0]. Compression retains
       * every first byte, both hex bytes for escapes, and no fourth bytes. */
      first = _mm512_mask_set1_epi32(lanes, escape, '%');
      if(flags & SIMDURL_FORM)
        first = _mm512_mask_set1_epi32(first, spaces, '+');
      candidate = _mm512_or_si512(first, _mm512_or_si512(
        _mm512_slli_epi32(high, 8), _mm512_slli_epi32(low, 16)));
      keep = _mm512_movepi8_mask(_mm512_maskz_set1_epi32(escape, -1));
      keep = (keep & UINT64_C(0x6666666666666666)) |
             UINT64_C(0x1111111111111111);
      packed = _mm512_maskz_compress_epi8(keep, candidate);

      /* A block produces at most 48 bytes. At least 32 input bytes remain,
       * so even this padded 64-byte store fits the original 3*n capacity. */
      memcpy(dst, &packed, sizeof(packed));
      dst += 16 + 2 * (unsigned int)__builtin_popcount((unsigned int)escape);
    }
    src += 16;
    n -= 16;
  }
  *input = src;
  *remaining = n;
  *output = dst;
}

SIMDURL_DETAIL_TARGET_VBMI2
static inline void simdurl_detail_encode_vbmi2(
  const char **input, size_t *remaining, char **output, unsigned int flags)
{
  if(flags & SIMDURL_FORM)
    simdurl_detail_encode_vbmi2_mode(input, remaining, output, SIMDURL_FORM);
  else
    simdurl_detail_encode_vbmi2_mode(input, remaining, output, SIMDURL_URI);
}

/* AVX2 accelerates blocks of literal bytes. A mixed block uses the portable
 * encoder once for the whole block, avoiding repeated vector classification
 * for adjacent escapes on processors without byte compression. */
SIMDURL_DETAIL_TARGET_AVX2
SIMDURL_DETAIL_INLINE void simdurl_detail_encode_avx2_mode(
  const char **input, size_t *remaining, char **output, unsigned int flags)
{
  const char *src = *input;
  size_t n = *remaining;
  char *dst = *output;

  while(n >= 32) {
    __m256i bytes, letter, digit, raw, spaces;
    memcpy(&bytes, src, sizeof(bytes));
    letter = _mm256_sub_epi8(_mm256_or_si256(bytes, _mm256_set1_epi8(32)),
                             _mm256_set1_epi8('a'));
    digit = _mm256_sub_epi8(bytes, _mm256_set1_epi8('0'));
    raw = _mm256_or_si256(
      _mm256_cmpeq_epi8(letter, _mm256_min_epu8(letter, _mm256_set1_epi8(25))),
      _mm256_cmpeq_epi8(digit, _mm256_min_epu8(digit, _mm256_set1_epi8(9))));
    raw = _mm256_or_si256(raw, _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('-')));
    raw = _mm256_or_si256(raw, _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('.')));
    raw = _mm256_or_si256(raw, _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('_')));
    raw = _mm256_or_si256(raw, _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(
      (flags & SIMDURL_FORM) ? '*' : '~')));
    spaces = _mm256_setzero_si256();
    if(flags & SIMDURL_FORM) {
      spaces = _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(' '));
      raw = _mm256_or_si256(raw, spaces);
    }

    if((unsigned int)_mm256_movemask_epi8(raw) == 0xffffffffU) {
      if(flags & SIMDURL_FORM)
        bytes = _mm256_blendv_epi8(bytes, _mm256_set1_epi8('+'), spaces);
      memcpy(dst, &bytes, sizeof(bytes));
      dst += 32;
    }
    else {
      static const char hex[] = "0123456789ABCDEF";
      unsigned int i;
      for(i = 0; i < 32; ++i) {
        unsigned char c = (unsigned char)src[i];
        if(simdurl_detail_unreserved(c, flags))
          *dst++ = (char)c;
        else if((flags & SIMDURL_FORM) && c == ' ')
          *dst++ = '+';
        else {
          *dst++ = '%';
          *dst++ = hex[c >> 4];
          *dst++ = hex[c & 15];
        }
      }
    }
    src += 32;
    n -= 32;
  }
  *input = src;
  *remaining = n;
  *output = dst;
}

SIMDURL_DETAIL_TARGET_AVX2
static inline void simdurl_detail_encode_avx2(
  const char **input, size_t *remaining, char **output, unsigned int flags)
{
  if(flags & SIMDURL_FORM)
    simdurl_detail_encode_avx2_mode(input, remaining, output, SIMDURL_FORM);
  else
    simdurl_detail_encode_avx2_mode(input, remaining, output, SIMDURL_URI);
}
#endif

static inline void simdurl_detail_encode(const char **input, size_t *remaining,
                                        char **output, unsigned int flags)
{
#ifdef SIMDURL_DETAIL_X86
  if(simdurl_detail_has_vbmi2())
    simdurl_detail_encode_vbmi2(input, remaining, output, flags);
  else if(simdurl_detail_has_avx2())
    simdurl_detail_encode_avx2(input, remaining, output, flags);
#else
  (void)input;
  (void)remaining;
  (void)output;
  (void)flags;
#endif
}

#endif
