# Historical benchmarks

[Bencher history](https://bencher.dev/perf/simdurl) records the public `simdurl`
project. The [benchmark workflow](../.github/workflows/benchmarks.yml) builds on
GitHub Actions and executes on Bencher's `intel-v1` hosted hardware.

## CI configuration

The `benchmark` job uses the GitHub environment named `Benchmarking`. Its
`BENCHER_SECRET` secret must contain the project's Bencher run key. The workflow
maps that secret to the CLI's `BENCHER_API_KEY` environment variable only while
uploading the image and submitting the run. A user API key is not needed in CI.

Local `.env` and `.env.*` files are ignored by Git. The Docker build context
uses an allowlist, and the final image contains only the benchmark executables,
Python harness, runtime, and build metadata. Neither key is passed into the
image. Registry credentials use a temporary Docker configuration.

Pull requests run the exporter/range tests and a short container smoke check
without credentials or network access inside the container. These checks do
not publish performance measurements. Publishing is restricted to `main`.

The first run begins after this workflow reaches `main`. Each push enumerates
all new **first-parent mainline commits**, including intermediate commits in a
multi-commit push. Commits inside a merged feature branch are represented by the
merge commit. Runs are queued without canceling earlier work; each workflow
runs one benchmark job at a time. GitHub permits up to 100 pending workflows
with `queue: max`, and up to 256 commits in one matrix.

A failed benchmark is a failed workflow, never a fabricated zero measurement.
Rerun failed jobs or use the manual range below to recover gaps. Benchmark
performance does not gate releases; thresholds can be added after collecting
enough history to characterize ordinary variation.

## Backfilling and retries

Choose **Actions → Benchmarks → Run workflow** on `main`:

- Leave both inputs empty to benchmark the current commit.
- Set `until` to a full mainline commit SHA to benchmark that revision.
- Set `since` and `until` to full mainline SHAs to benchmark the range after
  `since`, through `until`, inclusive. Empty `until` uses the workflow's main
  commit.

Endpoints must belong to main's first-parent history. Split ranges larger than
256 commits. Bencher may retain additional observations when a commit is rerun.
Backfilled reports retain the requested commit SHA, but charts follow report
submission order rather than the original commit dates.

The harness comes from the workflow revision, while library headers and source come
from the requested commit. This holds the workload constant during a backfill.
Revisions before the ASCII lowercase and hex APIs were introduced cannot build
the complete current suite; missing measurements never mean zero performance.

## Measurement contract

The image pins GCC 14.2.0 and Python 3.12.12 by image digest. It compiles the
header-only benchmarks with `-std=c99 -O3 -DNDEBUG`, without `-march=native`.
The scalar variants additionally define `SIMDURL_DISABLE_SIMD=1`; ordinary
compiler vectorization remains enabled.

The ten executables produce 1,356 series: 72 codec, 264 byte validation,
180 form-scanning, and 840 ASCII lowercase/hex helper series. Codec and validation
use automatic and scalar header-only builds. Form scanning and helpers also
measure a separately compiled library, linked without LTO. Every case has five
samples.
Bencher stores median
**CPU-time nanoseconds per operation**, with observed minimum and maximum as
bounds; these bounds are not confidence intervals. Codec elapsed time is
converted to latency, so byte throughput is never mislabeled as operations/sec.

The `formscan/` series use fixed-mode, non-inlined wrappers for URI and form
decoding, with literal, plus-separated, percent-separated, and mixed inputs
from 16 to 16,384 bytes. Long-marker cases use 256-byte literal runs and omit
short inputs that would duplicate the literal case. The benchmark checks an
independent decoding oracle outside timing. Its `automatic`, `scalar`, and
`compiled` series keep the different calling and build arrangements separate.
The scalar executable is named `simdurl_bench_formscan_portable`, matching CMake.

Form scanning defaults to 100,000 iterations at 16/64 bytes, then scales to
`max(1, base_iterations / ceil(length / 64))` for larger inputs. Each sample
reports CPU nanoseconds per operation directly. Existing codec and validation
workloads, iteration counts, and metric names are unchanged; adding these new
series retains their existing testbed and comparison history. Form scanning
runs after the original workloads.

The `helpers/` series measure ASCII lowercase copy and exact in-place conversion,
plus lowercase and uppercase hex encoding. The full suite contains 140 workloads:
runtime lengths from 0 to 4,096 bytes, including 15/16/17, 31/32/33, and 63/64/65
boundaries, plus fixed-size hex calls at 16, 20, 32, and 64 bytes. Each workload
has an independent optimized C comparator and a `simdurl` implementation, across
`automatic`, `scalar`, and `compiled` builds: 140 × 2 × 3 = 840 series.
Filter by `helpers/` and operation/build when comparing their histories.

Helper samples default to 500,000 iterations, alternating comparator/library
order across five samples. Fixed-size wrappers expose the same constant length
and case to both sides. The `ascii_inplace_already_lowered` cases start with
already-lowercased data; input-reset copying is outside timing. Timings include
wrapper calls and checksum sampling. The C comparator assumes valid arguments
and permits compiler vectorization; the public API performs its usual checks.
The compiled build preserves a separate API call without LTO.

Helpers run after the existing families. Harness metadata is version 3; existing
series retain their names, workloads, iteration counts, and testbed. Appending
new helper series therefore preserves the earlier comparison history.

The exporter rejects incomplete output, duplicate cases, nonpositive or
nonfinite timing, failed subprocesses, and inconsistent checksums. Its case
matrix must be updated when the C harness changes.

The testbed is `intel-v1-gcc14-v1`. Increment its version when changing the
compiler, flags, measurement method, or workload semantics, so incompatible
measurements do not share a trend line. Keep case names stable when the workload
is unchanged.

Bencher's published `intel-v1` specification does not guarantee VBMI2.
Each job captures CPU features and the library's available encode/decode/validate
and helper backends for full SIMD blocks. Helper AVX2 dispatch starts at 64 bytes;
shorter inputs can use SSE2 or portable tails.
The initial hosted run exposed AVX2 encoding/validation and the portable
decoder, without VBMI2. Tracking the VBMI2 path requires a different capable
testbed. Hardware changes require a separate testbed or baseline.

## Results and evidence

Bencher retains the historical metrics by source commit. The workflow also
uploads 90-day GitHub artifacts containing the report, remote job output, image
digest, and harness revision. The remote job's stderr contains a
`simdurl_evidence` JSON document with compiler flags, backend selection, every
raw executable output, individual timing samples, and machine metadata.
Download these artifacts if you need your own longer-lived raw archive.

The Bencher CLI can also download an individual report as JSON. To export its
summary values as CSV (with the usual `BENCHER_API_KEY` environment variable):

```sh
bencher report view simdurl "$REPORT_UUID" > report.json
jq -r '
  (["benchmark", "measure", "value", "minimum", "maximum"] | @csv),
  (.results[][] | .benchmark.name as $name | .measures[] |
    [$name, .measure.slug, .metric.value, .metric.lower_value,
     .metric.upper_value] | @csv)
' report.json > report.csv
```

For individual samples and binary hashes, download the report's job with
`bencher job view simdurl "$JOB_UUID"`; its stderr contains `simdurl_evidence`.

The remote image has no network dependency. The hosted job has a five-minute
execution limit; compilation occurs beforehand on GitHub Actions.

## Local verification

No credentials are needed to build or validate the exporter:

```sh
python3 -m unittest discover -s scripts -p 'test_benchmark*.py'
cmake -S . -B build-bencher -DCMAKE_BUILD_TYPE=Release \
  -DSIMDURL_BUILD_TESTS=OFF -DSIMDURL_BUILD_BENCHMARKS=ON
cmake --build build-bencher --parallel
python3 scripts/benchmark.py --bin-dir build-bencher/benchmarks \
  --output-dir build-bencher/local-results --commit "$(git rev-parse HEAD)"
```

The output directory must be empty. The command emits Bencher Metric Format
JSON on stdout and saves the same metrics, raw output, metadata, and samples in
the directory. Optional `--codec-iterations`, `--codec-repeats`,
`--validation-iterations`, `--formscan-iterations`, and `--helper-iterations`
arguments support smoke checks. Very small iteration counts may round to zero
and are rejected.

To exercise the exact container locally:

```sh
docker build --platform linux/amd64 -f benchmarks/bencher/Dockerfile \
  --build-arg SOURCE_SHA="$(git rev-parse HEAD)" -t simdurl-bencher:local .
docker run --rm --network none simdurl-bencher:local \
  > build-bencher/results.json 2> build-bencher/evidence.json
```

Local timings belong in their own testbed; they are not interchangeable with
hosted measurements. If using the locally installed Bencher CLI for setup,
load `.env` into your shell explicitly. `BENCHER_API_KEY` is its standard
credential variable; the CI configuration deliberately uses
`BENCHER_SECRET` instead.

References: [Bencher image execution](https://bencher.dev/docs/explanation/images/),
[metric format](https://bencher.dev/docs/reference/bencher-metric-format/),
[runner specifications](https://bencher.dev/docs/explanation/testbeds/).
