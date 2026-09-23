# Historical benchmarks

[Bencher history](https://bencher.dev/perf/simdurl) records the public `simdurl`
project. The [benchmark workflow](../.github/workflows/benchmarks.yml) builds on
GitHub Actions and executes on Bencher's `intel-v1` hosted hardware.
The [Azure VBMI2 lane](azure-benchmark-plan.md) adds automatic, disposable Azure
VMs to the same per-commit workflow when configured. It publishes to a separate
testbed and uses one source commit per allocation.

## CI configuration

The `benchmark` job uses the GitHub environment named `Benchmarking`. Its
`BENCHER_SECRET` secret must contain the project's Bencher run key. The workflow
uses it with `docker/login-action` for registry authentication and maps it to
the CLI's `BENCHER_API_KEY` only while submitting the run. A user API key is not
needed in CI.

All three image builds use pinned `docker/setup-buildx-action` and
`docker/build-push-action` releases with explicit filesystem contexts. The
hosted lane pushes directly to Bencher's registry and submits the build action's
immutable image digest. Tooling and Azure builds load the image locally for
archive verification; their action inputs include `provenance: false`.
Workflow steps invoke [the image CLI](../scripts/benchmark_image.py) for archive
verification and loading. Python logic lives in scripts with unit tests, not
inline in workflow YAML.

Local `.env` and `.env.*` files are ignored by Git. The Docker build context
uses an allowlist, and the final image contains the benchmark executables,
native backend tests, Python harness, runtime, and build metadata. Neither key is passed into the
image. Registry credentials use a temporary Docker configuration, and the login
action logs out during its cleanup. The builder action removes its builder after
the build action has finished exporting its records.

Pull requests run the separate [Benchmark checks workflow](../.github/workflows/benchmark-tooling.yml)
with exporter/range tests, short fixed-sampling container smoke checks, and one
default calibrated core run, without credentials or network access inside the
container. These checks retain their raw evidence as a seven-day artifact and
do not publish performance results. The main benchmark workflow calls that
same validation workflow before selecting commits and publishing measurements.
This keeps execution and cleanup jobs out of PR checks while preserving the
validation gate on `main`. Publishing is restricted to `main`.

The first run begins after this workflow reaches `main`. Each push enumerates
all new **first-parent mainline commits**, including intermediate commits in a
multi-commit push. Commits inside a merged feature branch are represented by the
merge commit. Runs are queued without canceling earlier work; each testbed's
matrix executes one commit at a time, with the two testbeds operating independently.
GitHub permits up to 100 pending workflows
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

## Ten benchmarks by default

The default `--suite core` runs and publishes **ten series**, all using automatic
header-only dispatch. The C executables skip unselected workloads before timing;
portable and compiled executables are only run with `--suite full`.

The table gives exact existing names, with the common `/simdurl/automatic` suffix
omitted. Keeping these names retains their historical identity.

| Benchmark name prefix | Purpose |
| --- | --- |
| `codec/encode/URI/mixed/128` | Representative short URL encoding |
| `codec/decode/form/mixed/128` | Representative percent/plus decoding |
| `codec/encode/URI/literal/4096` | Literal scan/copy fast path |
| `codec/encode/URI/dense/4096` | Bulk escape expansion |
| `codec/decode/URI/dense/4096` | Bulk percent decoding |
| `formscan/form/plus_long/16384` | Repeated 256-byte literal runs between plus markers |
| `validate/C0_DEL_SPACE/valid/4096` | Full-buffer scanning with control/DEL/space checks |
| `helpers/ascii_copy/mixed_ascii/128/runtime` | Header/token lowercase conversion |
| `helpers/hex_lower/binary/32/fixed` | Constant-length digest encoding |
| `helpers/hex_lower/binary/4096/runtime` | Bulk hex expansion |

These are representative synthetic workloads, not a production traffic profile.
They protect distinct paths without publishing every length, policy, comparator,
and build arrangement. Previously published series remain in Bencher history;
select these ten in a saved plot to exclude inactive historical series.

## Measurement contract

The image pins GCC 15.2.0 and Python 3.14.7 by image digest in
[`Dockerfile`](../benchmarks/bencher/Dockerfile). It compiles with
`-std=c99 -O3 -DNDEBUG`, without `-march=native`. Function target attributes compile
the accelerated kernels; runtime CPU checks select available backends.

Every core case uses calibrated sampling by default: at least **100 ms of
discarded warmup**, then **20 accepted timing samples**. Calibration chooses
iterations separately for each case to target **100 ms of CPU time per sample**;
samples shorter than **50 ms** are discarded and the iteration count increased.
Warmup and calibration measurements do not contribute to the published score.
Bencher stores median **CPU-time nanoseconds per operation**, with observed
minimum and maximum as bounds; these bounds are not confidence intervals. Each
of the ten cases has its own median, rather than a combined score. Lower latency
is better.

The minimum sample duration avoids basing a result on a few hundred microseconds
of work. Timings retain the existing wrapper, status checks, barriers, and
checksum sampling. Helper and validation comparators still check correctness
outside timing in core mode.

Harness metadata version 5 records the sampling policy, selected suite, and
published benchmark count. Evidence retains each batch's actual iteration count
and CPU duration, including discarded warmup and calibration batches, alongside
the 20 accepted latency samples. Workloads, timer, compiler flags, and metric
names are unchanged; the sampling policy identifies the change within the
existing testbed history.

The exporter rejects incomplete output, duplicate cases, nonpositive or
nonfinite timing, failed subprocesses, and inconsistent repeated checksums.
The selected case matrices must match the C filters.

The hosted testbed remains `intel-v1-gcc15-v1`, retaining the earlier five-sample
results alongside calibrated measurements. Harness metadata records the sampling
policy for each run. GCC 14 measurements retain their separate history. Use a
new version when changing the compiler, flags, or workload semantics so
incompatible measurements do not share a trend line. Keep case names stable when
the workload is unchanged.

Bencher's published `intel-v1` specification does not guarantee VBMI2.
Each job captures CPU features and the library's available encode/decode/validate
and helper backends for full SIMD blocks. Helper AVX2 dispatch starts at 64 bytes;
shorter inputs can use SSE2 or portable tails.
The initial hosted run exposed AVX2 encoding/validation and the portable
decoder, without VBMI2. The Azure lane requires native VBMI2 execution and uses
the existing `azure-d2s-v6-gcc15-v1` testbed. Hardware changes require a separate
testbed or baseline.

## Reading regressions and improvements

Compare each case against earlier runs on the same testbed. Do not combine the
ten timings into one score: a regression in decoding could be hidden by faster
hex encoding. As an initial review policy, investigate repeatable slowdowns of
20% or more; calibrate that threshold after observing normal run-to-run variation.
This is a suggested review threshold, not an enabled release gate.

Longer samples and warmup reduce noise within one allocation; they do not remove
differences in effective CPU frequency or contention between newly allocated
VMs. Earlier Azure runs with identical benchmark binaries on the same CPU model
moved together by roughly 10–20% across allocations. Review the next automatic
calibrated reports before treating smaller changes as regressions. Repeating one
unchanged commit across fresh allocations can characterize any remaining host
variation; this is a separate qualification exercise, not part of every run.

For a SIMD change, run the before and after revisions with the same harness,
compiler, CPU, and iteration counts. Repeat the runs with alternating revision
order. A convincing gain persists across runs and exceeds the ordinary spread;
one lower sample is insufficient. Use the full suite to examine the affected
sizes and patterns after the core suite identifies a change. Correctness and
boundary coverage belong in tests, even when they are absent from the dashboard.

Bencher supports [percentage thresholds](https://bencher.dev/docs/explanation/thresholds/)
for alerts; configure them after collecting a stable baseline. Keep improvement
review separate from regression alerts so expected speedups do not become failures.

## Full diagnostic suite

`--suite full` retains 1,356 series: 72 codec, 264 validation, 180 form-scanning,
and 840 lowercase/hex helper series. The ten executables include automatic and
portable header-only variants plus separately compiled formscan/helpers without
LTO. Ordinary compiler vectorization remains enabled in portable variants.

The full formscan matrix covers URI/form, literal/plus/percent/mixed inputs and
16–16,384 bytes. Long-marker cases use 256-byte literal runs and omit duplicate
short-input cases. It checks an independent decoder outside timing.

The helper matrix covers runtime lengths 0–4,096, SIMD boundaries, and fixed
16/20/32/64-byte digests, with independent optimized C comparators. Samples
alternate comparator/library order. Fixed-size wrappers expose the same constants
to both variants. In-place lowercase inputs are already lowercased; no reset copy
is timed. Comparator loops assume valid arguments while the public API retains
its checks. Validation includes high bytes and early/late rejection, which are
reported as call latency because early rejection need not inspect the full input.

Use this suite for local investigations, not routine publication to the ten-case
dashboard. Direct C executable invocations retain full coverage by default;
`EXECUTABLE ITERATIONS --core` selects only that family's core cases.
The full suite defaults to `--sampling fixed`, preserving the original five
samples per case and fixed iteration counts. Codec and validation use 100,000
iterations per sample, helpers use 500,000, and form scanning scales a base
100,000 by `max(1, iterations // ceil(length / 64))`.

## Results and evidence

Bencher retains the historical metrics by source commit. The workflow also
uploads 90-day GitHub artifacts containing the report, remote job output, image
digest, and harness revision. The remote job's stderr contains a
`simdurl_evidence` JSON document with compiler flags, backend selection, every
raw executable output, individual timing samples, and machine metadata.
Calibrated runs also retain warmup, calibration, and accepted batch evidence.
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
the directory. Add `--suite full` with a fresh output directory for diagnostics.
Use `--sampling fixed` with optional `--codec-iterations`, `--codec-repeats`,
`--validation-iterations`, `--formscan-iterations`, and `--helper-iterations`
arguments for short smoke checks. Fixed sampling is for functionality checks
and diagnostics; do not publish these reduced runs as performance measurements.
Very small iteration counts may round to zero and are rejected.

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
