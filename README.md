# simdurl

Allocation-free URL component encoding, decoding, byte validation, ASCII
lowercase conversion, and hex encoding in C99, with a C++ compatible API.

The library accepts byte spans and writes into caller-owned buffers. It supports
URI components and `application/x-www-form-urlencoded` components, arbitrary
binary data, and in-place decoding.

## Use

```c
#include <simdurl.h>
#include <stdio.h>

int main(void)
{
  const char input[] = "hello world+";
  char encoded[3 * (sizeof(input) - 1)];
  char decoded[sizeof(encoded)];
  simdurl_result e = simdurl_encode(input, sizeof(input) - 1,
                                   encoded, sizeof(encoded), SIMDURL_FORM);
  simdurl_result d;
  if(e.status != SIMDURL_OK)
    return 1;
  fwrite(encoded, 1, e.written, stdout); /* hello+world%2B */
  d = simdurl_decode(encoded, e.written, decoded, sizeof(decoded), SIMDURL_FORM);
  return d.status == SIMDURL_OK ? 0 : 1;
}
```

Lengths are explicit: zero means empty. Neither operation appends a NUL
terminator. Use `result.written` as the output length. To make a C string, reserve
an additional byte and append the terminator yourself after checking success.

| Mode | Unescaped bytes | Encoded space | Literal `+` when decoding |
| --- | --- | --- | --- |
| `SIMDURL_URI` (`0`) | `A-Z a-z 0-9 - . _ ~` | `%20` | `+` |
| `SIMDURL_FORM` | `A-Z a-z 0-9 * - . _` | `+` | space |

URI mode uses the [RFC 3986 unreserved set](https://www.rfc-editor.org/rfc/rfc3986#section-2.3).
Form mode uses the [WHATWG form percent-encode set](https://url.spec.whatwg.org/#application/x-www-form-urlencoded-percent-encode-set).
Encode individual components, such as a query key or value. These functions do
not parse whole URLs or assemble form key/value pairs. Text encoding is the
caller's responsibility; UTF-8 input is treated as its constituent bytes.

Encoding emits uppercase hex. Decoding accepts either hex case, leaves malformed
or incomplete escapes unchanged, and decodes only once: `%2520` becomes `%20`.
In form mode `%2B` becomes `+`, not space.

## Check raw bytes

`simdurl_validate_bytes(input, length, checks)` scans a bounded byte span without
modifying it. Select any combination of these checks:

| Check | Forbidden bytes |
| --- | --- |
| `SIMDURL_CHECK_C0` | `0x00` through `0x1F`, including NUL |
| `SIMDURL_CHECK_DEL` | `0x7F` |
| `SIMDURL_CHECK_SPACE` | `0x20` |

```c
const char input[] = "https://example.com/a%20b";
simdurl_status status = simdurl_validate_bytes(
  input, sizeof(input) - 1,
  SIMDURL_CHECK_C0 | SIMDURL_CHECK_DEL | SIMDURL_CHECK_SPACE);
/* SIMDURL_OK: %20 consists of three allowed bytes. */
```

The result is `SIMDURL_OK` or `SIMDURL_REJECTED`. Unknown check bits, or NULL with
a nonzero length, return `SIMDURL_INVALID_ARGUMENT`. Empty input is accepted;
zero checks accept without reading input after validating arguments.

This scans raw bytes: it does not decode escapes, convert `+`, or stop at an
embedded NUL. Bytes `0x80` through `0xFF` are always allowed. It checks neither
URL syntax nor text encoding. The check constants are separate from the codec
flags; in particular, DEL rejection is explicit.

## ASCII lowercase conversion

`simdurl_ascii_lower(input, length, output, capacity)` copies a byte span while
converting ASCII `A-Z` to `a-z`. Exact in-place conversion is supported:

```c
char host[] = "EXAMPLE.COM";
simdurl_result result = simdurl_ascii_lower(
  host, sizeof(host) - 1, host, sizeof(host) - 1);
/* On success: result.written == 11, and host contains "example.com". */
```

Provide at least `length` output bytes. Only ASCII uppercase letters change;
embedded NUL and bytes `0x80-0xFF` pass through unchanged. Conversion is
locale-independent and does not perform Unicode case conversion. Input and
output must either be identical pointers or refer to nonoverlapping spans.

## Hex encoding

`simdurl_hex_encode(input, length, output, capacity, flags)` encodes every input
byte as two hex digits. Choose `SIMDURL_HEX_LOWER` or `SIMDURL_HEX_UPPER`:

```c
const char bytes[] = { 0, (char)0xab, (char)0xff };
char hex[2 * sizeof(bytes)];
simdurl_result result = simdurl_hex_encode(
  bytes, sizeof(bytes), hex, sizeof(hex), SIMDURL_HEX_LOWER);
/* On success: result.written == 6; hex contains the six bytes "00abff". */
```

`simdurl_hex_encode_bound(n)` returns the exact size, `2*n`, or `SIZE_MAX` on
overflow. The encoder rejects lengths above `SIZE_MAX/2` before accessing input.
Input and output must not overlap. This encodes plain hex, without a `%` prefix.

Both helpers return `simdurl_result { status, written }` and append no terminator.
A NULL input is valid only for zero length, and a NULL output only for zero
capacity. Insufficient capacity returns `SIMDURL_BUFFER_TOO_SMALL`; invalid
NULL/length pairs or hex flags return `SIMDURL_INVALID_ARGUMENT`. All argument
and capacity checks occur before writing, so either error leaves output
unchanged and sets `written` to zero. Successful calls modify only the reported
output span and read only the input span.

## Build and install

Requires CMake 3.20 or newer and a C99 compiler. Tests additionally require C++11.
Normal builds need neither Node.js nor network access.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /your/install/prefix
```

Consumers use the exported CMake package:

```cmake
find_package(simdurl CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE simdurl::simdurl)
```

Set `CMAKE_PREFIX_PATH` to your installation prefix if necessary. The default is
a static library; `-DBUILD_SHARED_LIBS=ON` builds a shared library. Using
`add_subdirectory` or CMake `FetchContent` exposes the same targets and leaves
simdurl's tests disabled by default when it is a subproject.

For header-only use:

```cmake
target_link_libraries(my_app PRIVATE simdurl::header_only)
```

This supplies `SIMDURL_HEADER_ONLY` and needs no library at link time. Without
CMake, define `SIMDURL_HEADER_ONLY` before including `<simdurl.h>` and make the
entire `include/` directory available. Alternatively, define
`SIMDURL_IMPLEMENTATION` in exactly one C or C++ source file and use normal
includes elsewhere. Do not combine both macros in one translation unit.

## Keeping hot calls small

Header-only mode exposes the implementation to the optimizer, including constant
flags and lengths. URL and form loops are specialized before iteration, keeping
form handling out of the URL loop. Short inputs use portable C tails.
There are no heap allocations, mutable dispatch tables, or initialization calls.

By default, x86 builds select SIMD at runtime and remain usable on older CPUs:

| Compiler / CPU | URL encoding | URL decoding | Byte validation, ASCII lowercase, hex encoding |
| --- | --- | --- | --- |
| GCC 9+ or Clang 10+, x86-64 with AVX-512 VBMI2/BW/VL, AVX2, POPCNT | Parallel hex expansion and byte compression | Parallel substitution and byte compression | AVX2 / SSE2 |
| Same compilers, x86-64 with AVX2 only | SIMD literal blocks, scalar escapes | Portable C | AVX2 / SSE2 |
| Same compilers, other x86-64 CPUs | Portable C | Portable C | SSE2 |
| Other CPUs and compilers, including MSVC and clang-cl | Portable C | Portable C | Portable C |

The compiler versions above control whether SIMD implementations are compiled.
CI checks current GCC, Clang, Apple Clang, and MSVC. No special alignment is
required. The byte scanner uses explicit SIMD for 32-byte AVX2 or 16-byte SSE2
blocks, with bounded portable C tails. Its portable implementation uses 32-byte
reductions that the compiler may vectorize. Each path checks a block before
advancing, allowing early rejection without scanning the rest of the input.

Lowercase conversion and hex encoding also use explicit SSE2/AVX2, with fixed
16-/32-byte loads and stores for complete vector blocks and portable C tails.
Hex encoding expands each block into two output vectors and also handles an
eight-byte remainder with a 16-byte store. Both helpers select AVX2 starting at
64 input bytes, keeping shorter calls in SSE2/portable code to avoid dispatch
overhead. Their portable C loops
permit compiler-generated SIMD; actual vectorization depends on the compiler,
length, and its alias checks. Short calls and tails may remain scalar.

For a known deployment CPU, compile header-only callers with suitable target
flags (for example, `-march=native` on GCC/Clang x86-64). When all required
features are enabled at compile time, CPU checks fold away. Those binaries
require the selected CPU features. `-DSIMDURL_NATIVE=ON` applies native tuning to
the library and header-only users in the same build; installed header-only users
select their own CPU flags.

For a compiled static library, `-DSIMDURL_ENABLE_IPO=ON` enables interprocedural
optimization. Enable IPO on the consuming application too, with a compatible
compiler and linker, for optimization across the library boundary. IPO is
optional because compiler-specific LTO objects are not always suitable for
redistribution. `-DSIMDURL_DISABLE_SIMD=ON` disables explicit SIMD backends;
manual/header-only users can define `SIMDURL_DISABLE_SIMD` before inclusion.
Compiler-generated SIMD and optimized C library routines remain permitted in
the portable paths.

`simdurl_encode_bound(n)` returns `3*n`, or `SIZE_MAX` on overflow. Decoding needs
at most `n` output bytes. Providing those worst-case capacities enables SIMD
without a sizing pass. Smaller buffers also work when the actual result fits,
using a checked scalar path. Encoding rejects lengths above `SIZE_MAX/3` before
accessing input.

## API contract

URL encoding and decoding take
`(input, input_length, output, output_capacity, flags)` and return
`simdurl_result { status, written }`. The contracts for byte validation, ASCII
lowercase conversion, and hex encoding are described above.

| Status | Meaning |
| --- | --- |
| `SIMDURL_OK` | Success; `written` is the output byte count |
| `SIMDURL_BUFFER_TOO_SMALL` | Output does not fit |
| `SIMDURL_INVALID_ARGUMENT` | Invalid flags, invalid NULL/length pair, or encode length overflow |
| `SIMDURL_REJECTED` | A decoded or raw byte was forbidden by rejection flags |

For URL encoding and decoding, errors set `written` to zero and may leave the
destination partially modified.
SIMD stores may modify unused bytes within `output_capacity`; no write exceeds
capacity and no read exceeds `input_length`. If decoded input is forbidden and
output capacity is also insufficient, either `SIMDURL_REJECTED` or
`SIMDURL_BUFFER_TOO_SMALL` may be returned. A NULL input is accepted only with
zero input length, and a NULL output only with zero output capacity.

Encoding requires nonoverlapping buffers. Decoding permits exact in-place
operation (`input == output`); other overlap is unsupported. Functions have no
shared mutable state and can run concurrently with independent buffers.

Decode flags can combine `SIMDURL_FORM` with `SIMDURL_REJECT_NUL` or
`SIMDURL_REJECT_CONTROL`. These check raw and decoded bytes alike. Control
rejection covers bytes below `0x20`, including NUL, but accepts DEL (`0x7F`).
Encoding accepts only `SIMDURL_URI` or `SIMDURL_FORM`.

## Validation

The [safety testing guide](docs/testing.md) maps API guarantees to tests and
explains boundary matrices, backend execution requirements, sanitizer builds,
fuzzing, and release evidence. `SIMDURL_TEST_EXTENDED=ON` expands the memory
matrix; `SIMDURL_TEST_REQUIRE_BACKEND=vbmi2` makes that backend's execution a
requirement on a capable runner. `SIMDURL_BUILD_FUZZERS=ON` adds opt-in Clang
libFuzzer targets and corpus replay tests.

CTest exercises the compiled library, header-only and forced-portable variants,
C++ translation units (including a C caller linked to a C++ implementation),
exhaustive byte/hex cases, randomized reference checks,
unaligned and in-place buffers, bounds checks, and protected-page boundaries on
Unix and Windows. Scanner tests cover all 256 bytes and all eight check
combinations, every vector lane, direct backend calls, exact allocations, and
read-only guarded pages on Unix and Windows. Tests also install to a temporary
prefix, relocate that prefix, and build separate consumers of both CMake targets.
Form literal-scanning tests check linear search work to catch repeated suffix
scans without relying on timing thresholds.
Lowercase/hex tests check all byte values, exact in-place lowercase conversion,
both hex cases, preflight failures, vector boundaries, output canaries, direct
backends, and protected input/output pages.

CI covers Linux GCC/Clang, macOS ARM64, and Windows MSVC, MSYS2, and Cygwin,
including native 32-bit Windows builds. It checks static/shared libraries,
installed CMake consumers, native tuning, IPO, and sanitizers. A dedicated
Intel SDE job requires the VBMI2 encoder and decoder to execute. Setting
`-DSIMDURL_TEST_REQUIRE_BACKEND=vbmi2` makes backend tests fail when VBMI2 is unavailable.
ARM, MSVC, and 32-bit builds use the portable fallback. Performance depends on CPU, input
length, escape density, and compiler settings; measure on your deployment
workload.

Optional benchmarks compare the same header-only workload with runtime SIMD
selection and with explicit SIMD disabled:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSIMDURL_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./build/benchmarks/simdurl_bench
./build/benchmarks/simdurl_bench_scalar
```

Pass an iteration count, such as `100`, for a quick smoke run. Each executable
covers URI/form encoding/decoding, lengths 16/128/4096, and literal, mixed, and
dense escape patterns. Matching checksums help verify equivalent output. Throughput
is measured in input bytes per second.

The byte scanner has a separate benchmark against an independent portable C
branch loop, with ordinary compiler optimization enabled for both:

```sh
./build/benchmarks/simdurl_bench_validate
./build/benchmarks/simdurl_bench_validate_scalar
```

It covers lengths from 0 to 4096 bytes, valid and high-byte inputs, and early/late
rejection for control/DEL checks with and without space rejection. CSV output
reports five samples in nanoseconds per call; early exits do not process the
whole buffer. An optional argument sets iterations per sample (default 100000).
The `_scalar` target disables explicit SIMD, while compiler vectorization remains
permitted. Measurements use header-only calls with constant check flags.

Form literal scanning has a separate benchmark for repeated short and long
literal runs separated by `+` or percent escapes, mixed input, and plain literals:

```sh
./build/benchmarks/simdurl_bench_formscan
./build/benchmarks/simdurl_bench_formscan_portable
./build/benchmarks/simdurl_bench_formscan_compiled
```

These compare automatic header-only dispatch, portable C/libc, and compiled
library calls. URI controls use the same inputs. CSV output reports five samples
in CPU nanoseconds per call for 16 through 16384 input bytes. The optional argument
sets iterations at 64 bytes (default 20000); longer cases use fewer iterations.
Portable builds still permit compiler-generated SIMD and optimized libc routines.

Lowercase conversion and hex encoding have benchmarks for header-only,
portable C, and compiled-library calls:

```sh
./build/benchmarks/simdurl_bench_helpers
./build/benchmarks/simdurl_bench_helpers_portable
./build/benchmarks/simdurl_bench_helpers_compiled
```

These compare against simple C loops with normal compiler optimization enabled.
They cover short and long buffers, mixed/unchanged ASCII, high bytes, and both
hex cases. Separate fixed-length hex calls cover 16-, 20-, 32-, and 64-byte
digests so constant-length optimization is measured as well as runtime lengths.
In-place lowercase measurements use already-lowercased input, as labeled in the
CSV; they do not include an input-reset copy. Timings include call and checksum
costs, and the compiled variant includes its library call. Pass an iteration
count to override the default 100000 per sample. Bencher records all three
variants, including the 63/64/65-byte dispatch boundary, using 500000 iterations
per sample for helper measurements.

The [historical benchmarking guide](docs/benchmarking.md) describes the Bencher
workflow, per-commit results, manual backfills, and local verification.

## Releases

[GitHub Releases](https://github.com/cmeister2/simdurl/releases) provides source
archives named `simdurl-<version>.tar.gz` and their `SHA256SUMS` checksums. Version
tags use the form `1.2.3`.

Use the release archive for sources with versioned CMake package metadata.
Repository checkouts use `0.0.0` as their development version.

## License

The project is licensed under the [MIT License](LICENSE).
