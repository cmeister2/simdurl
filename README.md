# simdurl

Allocation-free URL component encoding and decoding in C99, with a C++ compatible
API.

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
form handling out of the URL loop. Short inputs go straight to scalar code.
There are no heap allocations, mutable dispatch tables, or initialization calls.

By default, x86 builds select SIMD at runtime and remain usable on older CPUs:

| Compiler / CPU | Encoding | Decoding |
| --- | --- | --- |
| GCC 9+ or Clang 10+, x86-64 with AVX-512 VBMI2/BW/VL, AVX2, POPCNT | Parallel hex expansion and byte compression | Parallel substitution and byte compression |
| Same compilers, x86-64 with AVX2 only | SIMD literal blocks, scalar escapes | Scalar |
| Other CPUs and compilers, including MSVC and clang-cl | Scalar | Scalar |

The compiler versions above control whether SIMD implementations are compiled.
CI checks current GCC, Clang, Apple Clang, and MSVC. No special alignment is
required.

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
redistribution. `-DSIMDURL_DISABLE_SIMD=ON` forces scalar code; manual/header-only
users can define `SIMDURL_DISABLE_SIMD` before inclusion.

`simdurl_encode_bound(n)` returns `3*n`, or `SIZE_MAX` on overflow. Decoding needs
at most `n` output bytes. Providing those worst-case capacities enables SIMD
without a sizing pass. Smaller buffers also work when the actual result fits,
using a checked scalar path. Encoding rejects lengths above `SIZE_MAX/3` before
accessing input.

## API contract

Both operations take `(input, input_length, output, output_capacity, flags)` and
return `simdurl_result { status, written }`.

| Status | Meaning |
| --- | --- |
| `SIMDURL_OK` | Success; `written` is the output byte count |
| `SIMDURL_BUFFER_TOO_SMALL` | Output does not fit |
| `SIMDURL_INVALID_ARGUMENT` | Invalid flags, invalid NULL/length pair, or encode length overflow |
| `SIMDURL_REJECTED` | A decoded or raw byte was forbidden by rejection flags |

On any error, `written` is zero and the destination may be partially modified.
SIMD stores may modify unused bytes within `output_capacity`; no write exceeds
capacity and no read exceeds `input_length`. A NULL input is accepted only with
zero input length, and a NULL output only with zero output capacity.

Encoding requires nonoverlapping buffers. Decoding permits exact in-place
operation (`input == output`); other overlap is unsupported. Functions have no
shared mutable state and can run concurrently with independent buffers.

Decode flags can combine `SIMDURL_FORM` with `SIMDURL_REJECT_NUL` or
`SIMDURL_REJECT_CONTROL`. These check raw and decoded bytes alike. Control
rejection covers bytes below `0x20`, including NUL, but accepts DEL (`0x7F`).
Encoding accepts only `SIMDURL_URI` or `SIMDURL_FORM`.

## Validation

CTest exercises the compiled library, header-only and forced-scalar variants,
C++ translation units, exhaustive byte/hex cases, randomized reference checks,
unaligned and in-place buffers, bounds checks, and protected-page boundaries on
supported Unix systems. It also installs to a temporary prefix, relocates that
prefix, and builds separate consumers of both CMake targets.

CI covers Linux GCC/Clang, macOS ARM64, and Windows MSVC; shared/static builds,
native tuning, IPO, and sanitizer configurations are included. ARM and MSVC use
the portable fallback. Performance depends on CPU, input length, escape density,
and compiler settings; measure on your deployment workload.

Optional benchmarks compare the same header-only workload with runtime SIMD
selection and with SIMD disabled:

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

## Releases

The setup follows the author's existing `deekay` and
`docker-credential-acr-login` projects: semantic-release, Conventional Commits,
and plain version tags such as `1.2.3`. A successful push to `main` runs the build
matrix, sanitizers, and source archive checks before releasing.

`fix:` commits cause patch releases, `feat:` commits cause minor releases, and
breaking changes (`!` or a `BREAKING CHANGE:` footer) cause major releases.
semantic-release makes the initial release `1.0.0`; the project version in
`CMakeLists.txt` is `0.0.0` until then. Use an initial `feat:` commit to trigger
the first release.

Release preparation updates the project version in `CMakeLists.txt`, rebuilds
and tests, and creates `simdurl-<version>.tar.gz` plus `SHA256SUMS`. A release
commit records `CMakeLists.txt` before semantic-release tags it and publishes
the source archive as a GitHub release. No npm package is published. GitHub's
default `GITHUB_TOKEN` needs contents write permission; branch protection must permit the release job to
push its version commit. Issue and pull-request comments are disabled.

Node.js 24.10+ is needed only for release tooling:

```sh
npm ci --ignore-scripts
npm run release:dry-run
```

The dry run requires a committed Git checkout and access to the configured GitHub
repository. To test archive preparation without tagging or publishing, run
`npm run release:prepare -- 0.0.0` from a tracked Git checkout; this sets
the CMake project version to the supplied value. Archives include only tracked
files.

## License

The project is licensed under the [MIT License](LICENSE).
