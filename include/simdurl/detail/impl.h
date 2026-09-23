/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Internal implementation; include <simdurl.h> instead.
 */
#ifndef SIMDURL_DETAIL_IMPL_H
#define SIMDURL_DETAIL_IMPL_H

#include "common.h"
#include "encode.h"
#include "decode.h"
#include "scan.h"

SIMDURL_DETAIL_INLINE simdurl_result
simdurl_detail_encode_tail(const char *input, size_t remaining, char *output,
                          size_t capacity, unsigned int flags,
                          size_t written, int checked)
{
  static const char hex[] = "0123456789ABCDEF";
  while(remaining) {
    unsigned char c = (unsigned char)*input++;
    size_t count;
    int raw = simdurl_detail_unreserved(c, flags);
    int space = (flags & SIMDURL_FORM) && c == ' ';
    count = (raw || space) ? 1 : 3;
    if(checked && count > capacity)
      return simdurl_detail_result(SIMDURL_BUFFER_TOO_SMALL, 0);
    if(raw || space)
      *output++ = space ? '+' : (char)c;
    else {
      output[0] = '%';
      output[1] = hex[c >> 4];
      output[2] = hex[c & 15];
      output += 3;
    }
    capacity -= count;
    written += count;
    --remaining;
  }
  return simdurl_detail_result(SIMDURL_OK, written);
}

SIMDURL_DETAIL_INLINE simdurl_result
simdurl_detail_decode_tail(const char *input, size_t remaining, char *output,
                          size_t capacity, unsigned int form,
                          unsigned char reject_limit, size_t written)
{
  while(remaining) {
    unsigned char c = (unsigned char)*input;
    size_t consumed = 1;
    if(c == '%') {
      if(remaining >= 3) {
        unsigned int h1 = simdurl_detail_hex((unsigned char)input[1]);
        unsigned int h2 = simdurl_detail_hex((unsigned char)input[2]);
        if((h1 | h2) < 16) {
          c = (unsigned char)((h1 << 4) | h2);
          consumed = 3;
        }
      }
    }
    else if(form && c == '+')
      c = ' ';
    else {
      /* A libc scan/copy keeps the scalar fallback fast on literal runs. */
      size_t count = simdurl_detail_literal_length(input, remaining, form);
      if(count > capacity)
        return simdurl_detail_result(SIMDURL_BUFFER_TOO_SMALL, 0);
      if(!simdurl_detail_literals_allowed(input, count, reject_limit))
        return simdurl_detail_result(SIMDURL_REJECTED, 0);
      memmove(output, input, count);
      output += count;
      input += count;
      remaining -= count;
      capacity -= count;
      written += count;
      continue;
    }
    if(c < reject_limit)
      return simdurl_detail_result(SIMDURL_REJECTED, 0);
    if(!capacity)
      return simdurl_detail_result(SIMDURL_BUFFER_TOO_SMALL, 0);
    *output++ = (char)c;
    input += consumed;
    remaining -= consumed;
    --capacity;
    ++written;
  }
  return simdurl_detail_result(SIMDURL_OK, written);
}

#ifdef __cplusplus
extern "C" {
#endif

SIMDURL_API simdurl_status simdurl_validate_bytes(const char *input,
                                                size_t input_length,
                                                unsigned int checks)
{
  int rejected;
  if((checks & ~(unsigned int)(SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL |
                              SIMDURL_CHECK_SPACE)) ||
     (!input && input_length))
    return SIMDURL_INVALID_ARGUMENT;
  if(!checks || !input_length)
    return SIMDURL_OK;
#ifdef SIMDURL_DETAIL_X86
  if(input_length >= 32 && simdurl_detail_has_avx2())
    rejected = simdurl_detail_scan_avx2(input, input_length, checks);
  else
    rejected = simdurl_detail_scan_sse2(input, input_length, checks);
#else
  rejected = simdurl_detail_scan_portable(input, input_length, checks);
#endif
  return rejected ? SIMDURL_REJECTED : SIMDURL_OK;
}

SIMDURL_API size_t simdurl_encode_bound(size_t input_length)
{
  return input_length > SIZE_MAX / 3 ? SIZE_MAX : input_length * 3;
}

SIMDURL_API simdurl_result simdurl_encode(const char *input, size_t input_length,
                                        char *output, size_t output_capacity,
                                        unsigned int flags)
{
  char *next = output;
  size_t remaining = input_length;
  size_t written = 0;
  int checked;
  if((flags & ~(unsigned int)SIMDURL_FORM) ||
     (!input && input_length) || (!output && output_capacity) ||
     input_length > SIZE_MAX / 3)
    return simdurl_detail_result(SIMDURL_INVALID_ARGUMENT, 0);
  if(!input_length)
    return simdurl_detail_result(SIMDURL_OK, 0);
  if(!output_capacity)
    return simdurl_detail_result(SIMDURL_BUFFER_TOO_SMALL, 0);
  checked = output_capacity < input_length * 3;
  if(!checked && remaining >= 32) {
    simdurl_detail_encode(&input, &remaining, &next, flags);
    written = (size_t)(next - output);
  }
  if(flags & SIMDURL_FORM) {
    if(checked)
      return simdurl_detail_encode_tail(input, remaining, next,
        output_capacity - written, SIMDURL_FORM, written, 1);
    return simdurl_detail_encode_tail(input, remaining, next,
      output_capacity - written, SIMDURL_FORM, written, 0);
  }
  if(checked)
    return simdurl_detail_encode_tail(input, remaining, next,
      output_capacity - written, SIMDURL_URI, written, 1);
  return simdurl_detail_encode_tail(input, remaining, next,
    output_capacity - written, SIMDURL_URI, written, 0);
}

SIMDURL_API simdurl_result simdurl_decode(const char *input, size_t input_length,
                                        char *output, size_t output_capacity,
                                        unsigned int flags)
{
  char *next = output;
  size_t remaining = input_length;
  size_t written = 0;
  unsigned char reject_limit;
  if((flags & ~(unsigned int)(SIMDURL_FORM | SIMDURL_REJECT_NUL |
                             SIMDURL_REJECT_CONTROL)) ||
     (!input && input_length) || (!output && output_capacity))
    return simdurl_detail_result(SIMDURL_INVALID_ARGUMENT, 0);
  if(!input_length)
    return simdurl_detail_result(SIMDURL_OK, 0);
  if(!output_capacity)
    return simdurl_detail_result(SIMDURL_BUFFER_TOO_SMALL, 0);
  reject_limit = (flags & SIMDURL_REJECT_CONTROL) ? 0x20 :
                 (flags & SIMDURL_REJECT_NUL) ? 1 : 0;
#ifdef SIMDURL_DETAIL_X86
  if(remaining >= 32 && output_capacity >= input_length &&
     simdurl_detail_has_vbmi2()) {
    if(!simdurl_detail_decode_vbmi2(&input, &remaining, &next, reject_limit,
                                   flags, input == output))
      return simdurl_detail_result(SIMDURL_REJECTED, 0);
    written = (size_t)(next - output);
  }
#endif
  if(flags & SIMDURL_FORM)
    return simdurl_detail_decode_tail(input, remaining, next,
      output_capacity - written, 1, reject_limit, written);
  return simdurl_detail_decode_tail(input, remaining, next,
    output_capacity - written, 0, reject_limit, written);
}

#ifdef __cplusplus
}
#endif
#endif
