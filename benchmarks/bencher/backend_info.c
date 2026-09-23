/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 * Describe dispatch for full SIMD blocks; shorter inputs still use C tails.
 */
#include <simdurl.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
  const char *encode = "portable", *decode = "portable", *validate = "portable";
  if(argc > 2 || (argc == 2 && strcmp(argv[1], "--require-vbmi2") != 0)) {
    fprintf(stderr, "Usage: %s [--require-vbmi2]\n", argv[0]);
    return 1;
  }
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
  if(argc == 2 && (strcmp(encode, "vbmi2") != 0 ||
                   strcmp(decode, "vbmi2") != 0)) {
    fprintf(stderr, "VBMI2 encoding and decoding are required; available "
                    "dispatch: encode=%s decode=%s\n", encode, decode);
    return 1;
  }
  printf("{\"compiler\":\"%s\",\"encode\":\"%s\",\"decode\":\"%s\",\"validate\":\"%s\","
         "\"ascii_lower\":\"%s\",\"hex_encode\":\"%s\"}\n",
         __VERSION__, encode, decode, validate, validate, validate);
  return 0;
}
