/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Deliberately independent of production classification and SIMD helpers.
 */
#ifndef SIMDURL_FUZZ_REFERENCE_H
#define SIMDURL_FUZZ_REFERENCE_H

#include <stddef.h>
#include <stdint.h>

#if defined(FUZZ_ENCODE)
static size_t reference_encode(const uint8_t *input, size_t length,
                               char *output, unsigned int flags)
{
  static const char hex[] = "0123456789ABCDEF";
  static const char uri[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  static const char form[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789*-._";
  const char *allowed = (flags & 1) ? form : uri;
  size_t i, j, written = 0;
  for(i = 0; i < length; ++i) {
    int unescaped = 0;
    for(j = 0; allowed[j]; ++j)
      if(input[i] == (uint8_t)allowed[j])
        unescaped = 1;
    if(unescaped)
      output[written++] = (char)input[i];
    else if(input[i] == ' ' && (flags & 1))
      output[written++] = '+';
    else {
      output[written++] = '%';
      output[written++] = hex[input[i] / 16];
      output[written++] = hex[input[i] % 16];
    }
  }
  return written;
}
#endif

#if defined(FUZZ_DECODE)
static int reference_hex(uint8_t byte)
{
  static const char lower[] = "0123456789abcdef";
  static const char upper[] = "0123456789ABCDEF";
  int i;
  for(i = 0; i < 16; ++i)
    if(byte == (uint8_t)lower[i] || byte == (uint8_t)upper[i])
      return i;
  return -1;
}

static size_t reference_decode(const uint8_t *input, size_t length,
                               char *output, unsigned int flags, int *rejected)
{
  size_t i, written = 0;
  *rejected = 0;
  for(i = 0; i < length; ++i) {
    uint8_t decoded = input[i];
    if(decoded == '%' && length - i >= 3) {
      int high = reference_hex(input[i + 1]);
      int low = reference_hex(input[i + 2]);
      if(high >= 0 && low >= 0) {
        decoded = (uint8_t)(high * 16 + low);
        i += 2;
      }
    }
    else if(decoded == '+' && (flags & 1))
      decoded = ' ';
    if(((flags & 2) && decoded == 0) || ((flags & 4) && decoded < 32))
      *rejected = 1;
    output[written++] = (char)decoded;
  }
  return written;
}
#endif
#endif
