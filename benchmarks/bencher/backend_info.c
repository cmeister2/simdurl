/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Describe dispatch for full SIMD blocks; shorter inputs still use C tails.
 */
#include <simdurl.h>
#include <stdio.h>

int main(void)
{
  const char *encode = "portable", *decode = "portable", *validate = "portable";
#ifdef SIMDURL_DETAIL_X86
  validate = "sse2";
  if(simdurl_detail_has_avx2()) {
    encode = "avx2";
    validate = "avx2";
  }
  if(simdurl_detail_has_vbmi2()) {
    encode = "vbmi2";
    decode = "vbmi2";
  }
#endif
#if defined(SIMDURL_BENCH_HAS_VALIDATION) && !SIMDURL_BENCH_HAS_VALIDATION
  validate = "unavailable";
#endif
  printf("{\"compiler\":\"%s\",\"encode\":\"%s\",\"decode\":\"%s\",\"validate\":\"%s\"}\n",
         __VERSION__, encode, decode, validate);
  return 0;
}
