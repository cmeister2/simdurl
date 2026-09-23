/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_DECODE_H
#define SIMDURL_DETAIL_DECODE_H

#ifdef SIMDURL_DETAIL_X86
/* Return a scalar mask, avoiding GCC's under-aligned Win64 vector return
 * slots (GCC PR54412). */
SIMDURL_DETAIL_TARGET_VBMI2
SIMDURL_DETAIL_INLINE unsigned int
simdurl_detail_special_mask(const char *input, unsigned int form)
{
  __m256i src;
  unsigned int mask;
  memcpy(&src, input, sizeof(src));
  mask = (unsigned int)_mm256_cmpeq_epi8_mask(src, _mm256_set1_epi8('%'));
  if(form)
    mask |= (unsigned int)_mm256_cmpeq_epi8_mask(src, _mm256_set1_epi8('+'));
  return mask;
}

/* Substitute decoded bytes at '%' positions, then compress away the two
 * hex digits. Escapes cannot overlap because '%' is not a hex digit. */
SIMDURL_DETAIL_TARGET_VBMI2
SIMDURL_DETAIL_INLINE int
simdurl_detail_decode_blocks(const char **input, size_t *remaining,
                            char **output, unsigned char reject_limit,
                            unsigned int form, int in_place)
{
  const char *string = *input;
  size_t alloc = *remaining;
  char *ns = *output;
  const char *next_percent = string;

  while(alloc >= 32) {
    __m256i src;
    unsigned int percent;
    memcpy(&src, string, sizeof(src));
    percent = (unsigned int)_mm256_cmpeq_epi8_mask(src, _mm256_set1_epi8('%'));
    if(!percent) {
      /* Use libc after proving a 128-byte literal prefix. In form mode a
       * literal '+' ends that prefix too, since it needs conversion. */
      unsigned int plus = form ? (unsigned int)_mm256_cmpeq_epi8_mask(
        src, _mm256_set1_epi8('+')) : 0;
      if(!plus && alloc >= 128 &&
         !simdurl_detail_special_mask(string + 32, form) &&
         !(simdurl_detail_special_mask(string + 64, form) |
           simdurl_detail_special_mask(string + 96, form))) {
        size_t n = 128 + simdurl_detail_literal_length(
          string + 128, alloc - 128, form, &next_percent);
        if(!simdurl_detail_literals_allowed(string, n, reject_limit))
          return 0;
        memmove(ns, string, n);
        ns += n;
        string += n;
        alloc -= n;
        continue;
      }
      if(reject_limit && _mm256_cmp_epu8_mask(
           src, _mm256_set1_epi8((char)reject_limit), _MM_CMPINT_LT))
        return 0;
      if(form)
        src = _mm256_mask_mov_epi8(src, (__mmask32)plus,
                                   _mm256_set1_epi8(' '));
      memcpy(ns, &src, sizeof(src));
      ns += 32;
      string += 32;
      alloc -= 32;
    }
    else {
      __m256i digit = _mm256_sub_epi8(src, _mm256_set1_epi8('0'));
      __m256i letter = _mm256_sub_epi8(
        _mm256_or_si256(src, _mm256_set1_epi8(32)), _mm256_set1_epi8('a'));
      __mmask32 digit_ok = _mm256_cmp_epu8_mask(
        digit, _mm256_set1_epi8(9), _MM_CMPINT_LE);
      __mmask32 letter_ok = _mm256_cmp_epu8_mask(
        letter, _mm256_set1_epi8(5), _MM_CMPINT_LE);
      unsigned int hex = (unsigned int)(digit_ok | letter_ok);
      unsigned int escapes = percent & (hex >> 1) & (hex >> 2);
      /* Defer escapes that might straddle the next 32-byte block. */
      unsigned int consumed = (percent & (1U << 30)) ? 30 :
                              (percent & (1U << 31)) ? 31 : 32;
      unsigned int keep = (0xffffffffU >> (32 - consumed)) &
                          ~((escapes << 1) | (escapes << 2));
      __m256i low = _mm256_and_si256(src, _mm256_set1_epi8(15));
      __m256i nibble = _mm256_mask_add_epi8(
        low, letter_ok, low, _mm256_set1_epi8(9));
      /* Bridge the two 16-byte halves for lane-local AVX2 shifts. */
      __m256i upper = _mm256_permute2x128_si256(nibble, nibble, 0x81);
      __m256i high_nibble = _mm256_alignr_epi8(upper, nibble, 1);
      __m256i low_nibble = _mm256_alignr_epi8(upper, nibble, 2);
      __m256i decoded = _mm256_or_si256(
        _mm256_and_si256(_mm256_slli_epi16(high_nibble, 4),
                         _mm256_set1_epi8((char)0xf0)), low_nibble);
      __m256i result = src;
      unsigned int count = (unsigned int)__builtin_popcount(keep);
      /* Replace literal '+' before inserting decoded bytes: %2B stays '+'. */
      if(form)
        result = _mm256_mask_mov_epi8(result,
          _mm256_cmpeq_epi8_mask(src, _mm256_set1_epi8('+')),
          _mm256_set1_epi8(' '));
      result = _mm256_mask_mov_epi8(result, (__mmask32)escapes, decoded);
      if(reject_limit && ((unsigned int)_mm256_cmp_epu8_mask(
           result, _mm256_set1_epi8((char)reject_limit), _MM_CMPINT_LT) & keep))
        return 0;
      result = _mm256_maskz_compress_epi8((__mmask32)keep, result);
      /* Separate buffers permit a full-vector store. In-place decoding must
       * preserve unconsumed bytes at positions 30 and 31. Keep the fixed-size
       * copy separate so compilers can emit a vector store. */
      if(in_place)
        memcpy(ns, &result, count);
      else
        memcpy(ns, &result, sizeof(result));
      ns += count;
      string += consumed;
      alloc -= consumed;
    }
  }
  *input = string;
  *remaining = alloc;
  *output = ns;
  return 1;
}

SIMDURL_DETAIL_TARGET_VBMI2
static int simdurl_detail_decode_vbmi2(const char **input, size_t *remaining,
                                     char **output, unsigned char reject_limit,
                                     unsigned int flags, int in_place)
{
  /* Specialize form behavior once, outside the vector loop. */
  if(flags & SIMDURL_FORM)
    return simdurl_detail_decode_blocks(input, remaining, output,
                                        reject_limit, 1, in_place);
  return simdurl_detail_decode_blocks(input, remaining, output,
                                      reject_limit, 0, in_place);
}
#endif
#endif
