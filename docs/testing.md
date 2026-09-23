# Safety and correctness testing

The suite checks the byte-span API, not URL syntax or text encoding. A passing
run is evidence for its compiler, optimization settings, CPU paths, and test
inputs; it is not a proof for all C implementations or all possible inputs.
Callers must supply valid spans and obey the overlap preconditions in
`include/simdurl.h`.

## Contract checklist

| Obligation | Primary tests |
| --- | --- |
| URI/form byte mapping, uppercase encoding, either-case hex decoding, malformed escapes, binary data | `test_simdurl.c`: examples, all bytes, all 65,536 escape pairs, independent references |
| Raw and decoded rejection flags, independent scanner checks | `test_simdurl.c`, `test_scanner.c`, direct backend tests |
| NULL/zero-length/zero-capacity combinations and every unknown flag bit | `test_contract.c` |
| Encode-bound overflow and argument validation before input access | `test_contract.c`, memory short-circuit tests |
| Independent source/destination alignment | `test_memory.c`, direct backend tests |
| Bounded reads, including vector tails | Exact allocations under ASan and leading/trailing guarded spans in `test_memory.c` and `test_scanner.c` |
| Bounded writes on success and errors | Capacity canaries, exact allocations, protected output boundaries |
| Input remains unchanged for separate-buffer operations | Input snapshots and read-only mappings |
| Exact in-place decoding preserves unread bytes | Memory tests, direct decoder tests, semantic reference comparisons |
| Every error has `written == 0` | Contract, memory, semantic, and fuzz tests |
| Portable and CPU-specific implementations agree with independent references | `test_backends.c`, direct scanner tests, fuzz targets |
| C/C++ and packaging modes | Compiled/header-only/portable suites, full C++ codec suite, single implementation TU with separate consumers, relocated install tests |

The oracle compares only the first `written` bytes on success. It never requires
an unchanged output buffer on errors. SIMD may change bytes after `written`
within `output_capacity`; the capacity boundary is the safety boundary.
If forbidden decoded data and insufficient output capacity coexist, either
`REJECTED` or `BUFFER_TOO_SMALL` is permitted, but success is not. With sufficient
capacity, rejection must be reported. Unsupported partial overlap is not a test
case that must be accepted or rejected safely.

Canaries alone cannot detect overreads, and an oversized initialized source
allocation can hide them from ASan. Guard pages and exact heap allocations
complement the independent alignment matrix. A page boundary fixes pointer
alignment relative to length; the alignment and guard matrices intentionally
exercise different dimensions. For in-place decoding with capacity shorter
than the input, the remaining readable source must stay accessible and unchanged;
canaries/snapshots enforce the write limit there.

## Running the suite

The ordinary build runs all deterministic suites, with assertions active in
Release builds. No external test framework or network access is required.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The extended memory matrix is opt-in for scheduled runs:

```sh
cmake -S . -B build-extended -DCMAKE_BUILD_TYPE=Release -DSIMDURL_TEST_EXTENDED=ON
cmake --build build-extended --parallel
ctest --test-dir build-extended --output-on-failure
```

Direct tests emit `backend` records identifying compiled, executed, and skipped
implementations. Retain verbose CTest logs to retain these successful records.
An unsupported CPU path is a skip, not execution evidence. On a designated
machine, require a backend explicitly:

```sh
cmake -S . -B build-required -DCMAKE_BUILD_TYPE=Release -DSIMDURL_TEST_REQUIRE_BACKEND=vbmi2
cmake --build build-required --parallel
ctest --test-dir build-required --verbose
```

Accepted requirements are `portable`, `sse2`, `avx2`, and `vbmi2`. Portable
applies to all operations; SSE2 to scanning; AVX2 to encoding/scanning; and
VBMI2 to encoding/decoding. A required but uncompiled/unsupported backend
fails the test. No requirement bypasses CPU feature checks. The optional CI
job is requested with `workflow_dispatch`, `require_vbmi2=true`, on `main`; it
requires a runner labeled `self-hosted`, `linux`, `x64`, and `simdurl-vbmi2`.
A requested job that fails or does not run fails the workflow. Runtime-dispatched
public calls are tested separately by the main suites. Run a baseline binary on
an older x86-64 CPU to validate the public fallback on that hardware; compiling
with SIMD disabled is additional coverage, not equivalent dispatch evidence.

For optimized address/undefined-behavior checking:

```sh
CC=clang CXX=clang++ cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure -E install_consumer
```

The ordinary install-consumer test does not propagate instrumentation flags to
external consumers, so run that test in the normal build. MemorySanitizer is a
separate job: use the C targets and a toolchain/runtime setup compatible with
MSan instrumentation and address-space mapping. Do not silence findings by
broadly ignoring library code. The sanitizer suites also test portable paths;
compiler-generated SIMD remains enabled unless explicitly disabled.

## Differential fuzzing

Clang/libFuzzer support is opt-in and does not affect installed targets:

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSIMDURL_BUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
ctest --test-dir build-fuzz --output-on-failure -R corpus
```

See `fuzz/README.md` for corpus layout and campaign commands. Use a writable
copy of the corpus for campaigns. Replay committed regressions on every PR;
retain and minimize failures before adding them as deterministic cases. The
harness varies flags, capacity, alignment, and supported in-place operation,
with bounded allocations and independent references. Repeat with
`SIMDURL_DISABLE_SIMD=ON` and on designated CPU runners. A finite fuzz campaign
is evidence, not a guarantee that all inputs were explored.

## Bounds review obligations

These arguments complement dynamic testing. Revisit them whenever a kernel,
dispatch threshold, or store width changes; they are not a machine-checked proof.
They assume valid C objects and representable pointer operations.

- The public encoder checks `input_length <= SIZE_MAX / 3` before multiplication.
  The unchecked vector path starts with capacity at least `3 * input_length`.
  Since output produced is at most three times input consumed, remaining
  capacity stays at least three times remaining input. The VBMI2 loop requires
  at least 32 input bytes before its 16-byte load and possible 64-byte store:
  at least 96 output bytes remain at that point. AVX2 consumes 32 and emits at
  most 96. Checked scalar tails verify capacity before each output unit.
- The vector decoder starts with capacity at least the original input length.
  Produced output never exceeds consumed input, so a 32-byte store fits while
  at least 32 input bytes remain. The 128-byte literal probe is separately
  guarded. Escapes starting at lanes 30/31 are deferred; in-place compression
  stores only its output count to preserve unread source. Exact-capacity calls
  below the SIMD threshold use the checked scalar path.
- Scanner loads are guarded by their 16/32-byte block lengths, and tails are
  bounded. The no-checks public path validates arguments before returning
  without reading input. SIMD loads use unaligned operations or `memcpy`.
- Literal scans/copies use bounded lengths. Byte classification uses unsigned
  byte values, and no mutable dispatch table or per-call shared state is needed.

## Release evidence

Keep CTest logs, backend execution records, sanitizer results, fuzz corpora and
failure artifacts, and source/branch coverage reports. Review coverage of
bounds checks, tails, dispatch, and rejection paths; explain unreachable or
platform-specific branches instead of presenting a single percentage as proof.
On a VBMI2-capable x86 host, expected uncovered outcomes include false CPU
feature checks and the encoder's public AVX2/fallback dispatch alternatives.
Direct kernel tests cover those kernels but cannot demonstrate dispatch on a
CPU lacking the features. Keep these exclusions attached to the report and
obtain the complementary run on the appropriate CPU; do not suppress the
branches merely to improve the percentage.
An ordinary hosted job may not execute VBMI2. Use the designated runner workflow
and retain its execution record before claiming validation of that backend for
a release. Historical logs from a different revision are not sufficient.

PR jobs run deterministic tests, sanitizer checks, and corpus replay plus
10-second fuzz campaigns per operation/mode. The Monday 03:23 UTC schedule runs
the extended optimized ASan memory matrix and 60-second campaigns per
operation/mode. Coverage reports use matching LLVM tools and local debug data;
external debuginfod lookup is disabled so reporting needs no debug-info server. CI retains existing
Linux/macOS/Windows, ARM64, 32-bit Windows, static/shared, native, and IPO coverage,
and adds signed/unsigned-char and non-vectorized portable builds. Hardware or
platform variants not actually executed must remain explicit limitations in a
release's validation record.
