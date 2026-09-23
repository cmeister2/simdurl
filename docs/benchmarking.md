# Historical benchmarks

[Bencher history](https://bencher.dev/perf/simdurl) records the public `simdurl`
project. The [benchmark workflow](../.github/workflows/benchmarks.yml) builds on
GitHub Actions and executes on Bencher's `intel-v1` hosted hardware.

## CI configuration

The `benchmark` job uses the GitHub environment named `Benchmarking`. Its
`BENCHER_SECRET` secret must contain the project's Bencher run key. The workflow
maps that secret to the CLI's `BENCHER_API_KEY` environment variable in the
image build, upload, and benchmark step. A user API key is not needed in CI.

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
merge commit. Runs are queued without canceling earlier work. Within each
workflow, one job submits commits oldest first and waits for each successful
report before
starting the next commit. GitHub permits up to 100 pending workflows with
`queue: max`. The planner accepts at most 256 commits per run; split long ranges
to fit the six-hour job limit. The execution step stops after 350 minutes to
leave time for cleanup and evidence upload.

A failed benchmark is a failed workflow, never a fabricated zero measurement.
A failure stops the range before later commits are submitted. Use the manual
range below to resume after the last successful commit; rerunning the entire
job repeats any completed observations. Benchmark performance does not gate
releases; thresholds can be added after collecting
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
Manual runs retain the requested commit SHA and use its committer timestamp
for the graph date. Automatic push runs use the measurement date. Bencher
version numbers still follow the order in which commit hashes were first
submitted.

The artifact records the selected range in `commits.txt`, per-commit evidence
in `<SHA>/`, and progress in `last-successful-commit.txt`. To resume a failed
range, set `since` to the last successful SHA and keep the original `until`.
If nothing succeeded, repeat the original range.

The harness comes from the workflow revision, while library headers come from
the requested commit. This holds the workload constant during a backfill.
The image detects whether those headers declare byte validation. Earlier
revisions run the 72 codec series; revisions with validation run all 336 series.
Unavailable validation measurements are omitted, and backend metadata records
validation as unavailable. The initial README-only commit has no library to
benchmark.

Bencher assigns version numbers when a previously unseen commit is first
submitted. Backdating alone cannot insert an older commit before an existing
version. To rebuild a chronological history, pause other main submissions,
preserve the existing reports, and start a fresh Bencher branch head with
`bencher branch update simdurl main --start-point-reset`. Existing reports remain
stored under the previous head. Submit one commit at a time, oldest first,
waiting for each successful report and checking its version and hash before
continuing. The workflow uses an explicit loop because a serialized matrix
does not guarantee job scheduling order.

The manual workflow supplies `--backdate` with each commit's committer
timestamp. Supply the same option when submitting historical runs locally.
These are measurements taken now against historical source; the job evidence retains the actual measurement timestamps.
Restore automatic submissions after catching up to the current main tip.

## Measurement contract

The image pins GCC 14.2.0 and Python 3.12.12 by image digest. It compiles the
header-only benchmarks with `-std=c99 -O3 -DNDEBUG`, without `-march=native`.
The scalar variants additionally define `SIMDURL_DISABLE_SIMD=1`; ordinary
compiler vectorization remains enabled.

The four executables produce 336 series: URI/form encoding and decoding,
and byte validation with its independent C comparator, across both automatic
SIMD and scalar builds. Every case has five samples. Bencher stores median
**CPU-time nanoseconds per operation**, with observed minimum and maximum as
bounds; these bounds are not confidence intervals. Codec elapsed time is
converted to latency, so byte throughput is never mislabeled as operations/sec.

The exporter rejects incomplete output, duplicate cases, nonpositive or
nonfinite timing, failed subprocesses, and inconsistent checksums. Its case
matrix must be updated when the C harness changes.

The testbed is `intel-v1-gcc14-v1`. Increment its version when changing the
compiler, flags, measurement method, or workload semantics, so incompatible
measurements do not share a trend line. Keep case names stable when the workload
is unchanged.

Bencher's published `intel-v1` specification does not guarantee VBMI2.
Each job captures CPU features and the library's available encode/decode/validate
backends for full SIMD blocks. Short inputs can still use portable tails.
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
the directory. Optional `--codec-iterations`, `--codec-repeats`, and
`--validation-iterations` arguments support smoke checks. Very small iteration
counts may round to zero and are rejected.
Use `--families codec` when running only the codec executables from an older
revision; the default is `--families codec validate`.

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
