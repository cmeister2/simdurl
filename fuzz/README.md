# Differential fuzzing

Build with Clang and its libFuzzer runtime (not required for normal builds):

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIMDURL_BUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
ctest --test-dir build-fuzz -L corpus --output-on-failure
mkdir -p /tmp/simdurl-corpus /tmp/simdurl-artifacts
cp -R fuzz/corpus/* /tmp/simdurl-corpus/
build-fuzz/fuzz/simdurl_fuzz_decode -max_len=4104 -max_total_time=300 \
  -artifact_prefix=/tmp/simdurl-artifacts/ /tmp/simdurl-corpus/decode
```

There are separate `simdurl_fuzz_encode`, `simdurl_fuzz_decode`, and
`simdurl_fuzz_scanner` targets. Each instruments the header-only library with
libFuzzer, ASan and fail-fast UBSan. Configure a second build with
`-DSIMDURL_DISABLE_SIMD=ON` to fuzz the portable implementation. Public dispatch
selects available SIMD implementations; direct deterministic backend tests and
their execution records establish which backends ran. A hosted fuzz run does
not imply AVX-512 coverage.

The committed binary seed corpus is replayed by CTest with `-runs=0`. Copy it to
a writable working corpus before a campaign: libFuzzer adds discoveries to its
first corpus directory. CI keeps working corpora and crash artifacts. To replay
a failure, run the matching executable with the artifact filename as its only
argument. A failure reports lengths, flags, capacity, offsets, and allocation
mode; libFuzzer preserves the exact input. Keep minimized failures here as
regression seeds, and add a deterministic test for the underlying invariant.

## Corpus format

The first eight bytes describe the call; the remaining bytes are the payload,
limited to 4096 bytes. Missing header bytes are zero. Numeric fields are bytes
unless stated otherwise.

| Byte | Meaning |
| --- | --- |
| 0 | Valid flags/checks (`& 1` for encode, `& 7` otherwise) |
| 1 | Source offset within allocation, modulo 64 |
| 2 | Destination offset within its independent allocation, modulo 64 |
| 3 | Capacity selector, modulo 8 (table below) |
| 4–5 | Little-endian capacity value for selector 5 |
| 6 | Bit 0 adds an invalid flag; bit 1 makes input NULL; bit 2 makes output NULL |
| 7 | Bit 0 selects an allocation ending exactly at the logical span; otherwise a poisoned canary suffix is added |

Capacity selectors are: 0 worst-case bound; 1 exact reference output size;
2 one below exact; 3 zero; 4 bound plus one; 5 supplied value modulo bound plus
two; 6 input length; 7 exact plus one. Scanner ignores output controls.
Decode also repeats every call with exact in-place storage, preserving bytes
beyond output capacity even when they are still readable input.

The scalar oracles use no production classification helpers. They check exact
successful output and length, argument errors, zero `written` on every error,
source preservation, and writes outside advertised capacity. Both documented
errors are accepted when rejected bytes and insufficient capacity coincide:
the API does not specify their precedence. Bytes between `written` and capacity
are intentionally unconstrained. ASan poisons alignment padding and logical
span tails; exact allocations also expose their ends to allocator redzones.
Readable in-place input beyond output capacity is compared after the call.

Seeds include empty input, all byte values, vector boundary lengths, URI/form
rules, dense and malformed escapes, rejection, tight/zero capacities, invalid
flags, and NULL arguments. Scheduled CI extends the same campaigns; fuzzing
provides evidence, not a proof that all possible executions are safe.
