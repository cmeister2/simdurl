/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Max Dymond
 */
#ifndef SIMDURL_H
#define SIMDURL_H

#include <stddef.h>
#include <stdint.h>

/* Define SIMDURL_HEADER_ONLY before inclusion for a dependency-free inline
 * implementation. Otherwise link simdurl, or define SIMDURL_IMPLEMENTATION in
 * exactly one C or C++ translation unit. Do not combine these two macros.
 * Define SIMDURL_DISABLE_SIMD to build only the portable scalar implementation.
 */
#if defined(SIMDURL_HEADER_ONLY) && defined(SIMDURL_IMPLEMENTATION)
#error "Choose SIMDURL_HEADER_ONLY or SIMDURL_IMPLEMENTATION"
#endif

#if defined(SIMDURL_HEADER_ONLY)
#define SIMDURL_API static inline
#elif (defined(_WIN32) || defined(__CYGWIN__)) && defined(SIMDURL_SHARED)
#if defined(SIMDURL_BUILD)
#define SIMDURL_API __declspec(dllexport)
#else
#define SIMDURL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(SIMDURL_SHARED)
#define SIMDURL_API __attribute__((visibility("default")))
#else
#define SIMDURL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum simdurl_status {
  SIMDURL_OK = 0,
  SIMDURL_BUFFER_TOO_SMALL,
  SIMDURL_INVALID_ARGUMENT,
  SIMDURL_REJECTED
} simdurl_status;

typedef struct simdurl_result {
  simdurl_status status;
  size_t written;
} simdurl_result;

enum simdurl_flags {
  SIMDURL_URI = 0,
  SIMDURL_FORM = 1,
  SIMDURL_REJECT_NUL = 2,
  SIMDURL_REJECT_CONTROL = 4
};

/* Worst-case encoded size, without a terminator; SIZE_MAX on overflow. */
SIMDURL_API size_t simdurl_encode_bound(size_t input_length);

/* Encode bytes as an RFC 3986 URI component (flags = SIMDURL_URI), or as an
 * application/x-www-form-urlencoded component (flags = SIMDURL_FORM).
 * URI leaves A-Z a-z 0-9 - . _ ~ unchanged. Form leaves A-Z a-z 0-9 * - . _
 * unchanged and encodes spaces as '+'. Other bytes become uppercase %HH.
 * Input and output must not overlap. Only URI and FORM are valid encode flags.
 */
SIMDURL_API simdurl_result simdurl_encode(const char *input, size_t input_length,
                                        char *output, size_t output_capacity,
                                        unsigned int flags);

/* Decode valid %HH escapes (either hex case), passing malformed escapes through.
 * FORM additionally converts literal '+' to space; %2B remains '+'. Optional
 * REJECT_NUL and REJECT_CONTROL reject raw OR decoded bytes: NUL or bytes below
 * 0x20 respectively. REJECT_CONTROL includes NUL but does not include DEL.
 * Exact in-place decoding (input == output) is supported; other overlap is not.
 * input_length bytes of output capacity always suffice.
 *
 * Both operations are allocation-free and binary-safe. Lengths are explicit:
 * zero means empty, and no NUL terminator is appended. A NULL input is valid
 * only for zero input length; a NULL output only for zero output capacity.
 * On success, written is the number of output bytes. On any error, written is
 * zero and output may be partially modified. Bytes after written but within
 * output_capacity may be modified by vector stores. No writes exceed capacity.
 * Exact output capacities work; worst-case capacity enables the SIMD path.
 */
SIMDURL_API simdurl_result simdurl_decode(const char *input, size_t input_length,
                                        char *output, size_t output_capacity,
                                        unsigned int flags);

#ifdef __cplusplus
}
#endif

#if defined(SIMDURL_HEADER_ONLY) || defined(SIMDURL_IMPLEMENTATION)
#include "simdurl/detail/impl.h"
#endif

#undef SIMDURL_API
#endif
