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
 * Define SIMDURL_DISABLE_SIMD to omit explicit SIMD backends. The portable C
 * implementation can still be vectorized by the compiler.
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

/* Raw-byte checks for simdurl_validate_bytes; independent of codec flags. */
enum simdurl_byte_checks {
  SIMDURL_CHECK_C0 = 1,
  SIMDURL_CHECK_DEL = 2,
  SIMDURL_CHECK_SPACE = 4
};

/* Validate a bounded span of raw bytes, with any combination of CHECK_C0
 * (0x00-0x1f), CHECK_DEL (0x7f), and CHECK_SPACE (0x20). Return REJECTED if
 * any selected byte occurs, otherwise OK. Bytes 0x80-0xff are always allowed.
 * No escapes are decoded, '+' is unchanged, and NUL is an ordinary byte.
 * Input is not modified and no bytes outside input_length are read.
 * Unknown check bits or NULL with nonzero length return INVALID_ARGUMENT.
 * An empty span is valid; zero checks accept without reading input after
 * validating arguments. This does not validate URL syntax or text encoding.
 */
SIMDURL_API simdurl_status simdurl_validate_bytes(const char *input,
                                                size_t input_length,
                                                unsigned int checks);

/* Convert only ASCII A-Z to a-z, copying all other bytes unchanged, including
 * NUL and bytes 0x80-0xff. Length is explicit; no terminator is appended.
 * Exact in-place conversion (input == output) is supported; other overlap is
 * not. Output capacity must be at least input_length. No read or write extends
 * beyond the input or written output span.
 * NULL input is valid only for zero length; NULL output only for zero capacity.
 * Errors return written = 0 and leave output unchanged. Success writes exactly
 * input_length bytes. This is locale-independent, not Unicode case conversion.
 */
SIMDURL_API simdurl_result simdurl_ascii_lower(const char *input,
                                             size_t input_length,
                                             char *output,
                                             size_t output_capacity);

enum simdurl_hex_case {
  SIMDURL_HEX_LOWER = 0,
  SIMDURL_HEX_UPPER = 1
};

/* Exact hex-encoded size, without a terminator; SIZE_MAX on overflow. */
SIMDURL_API size_t simdurl_hex_encode_bound(size_t input_length);

/* Encode each byte as two hex digits. Choose HEX_LOWER (0) or HEX_UPPER;
 * other flag bits are invalid. Input and output must not overlap.
 * Length is explicit; embedded NUL is data and no terminator is appended.
 * Output capacity must be at least 2 * input_length. Lengths above SIZE_MAX/2
 * return INVALID_ARGUMENT before accessing input or computing the product.
 * NULL input is valid only for zero length; NULL output only for zero capacity.
 * Errors return written = 0 and leave output unchanged. Success writes exactly
 * 2 * input_length bytes. No read or write extends beyond those spans.
 */
SIMDURL_API simdurl_result simdurl_hex_encode(const char *input,
                                            size_t input_length,
                                            char *output,
                                            size_t output_capacity,
                                            unsigned int flags);

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
 * URL encoding and decoding are allocation-free and binary-safe. Lengths are
 * explicit: zero means empty, and no NUL terminator is appended. A NULL input
 * is valid only for zero input length; a NULL output only for zero capacity.
 * On success, written is the number of output bytes. On any error, written is
 * zero and output may be partially modified. Bytes after written but within
 * output_capacity may be modified by vector stores. No writes exceed capacity.
 * If decoded input is forbidden and output capacity is also insufficient,
 * either REJECTED or BUFFER_TOO_SMALL may be returned.
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
