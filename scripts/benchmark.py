#!/usr/bin/env python3
"""Run the C benchmarks and emit Bencher Metric Format (latency in ns).

Only the final BMF document is written to stdout. Raw output, individual timing
samples, and explicitly selected machine metadata are retained in --output-dir.
The expected case matrices deliberately fail closed when the C harness changes.
"""

import argparse
import csv
import datetime
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time


HARNESS_VERSION = 2
VALIDATION_REPEATS = 5
FORMSCAN_REPEATS = 5
BUILDS = ("automatic", "scalar")
FORMSCAN_BUILDS = ("automatic", "scalar", "compiled")
CODEC_CASES = {
    (operation, mode, pattern, str(length))
    for operation in ("encode", "decode")
    for mode in ("URI", "form")
    for pattern in ("literal", "mixed", "dense")
    for length in (16, 128, 4096)
}
VALIDATION_CASES = {
    (checks, pattern, str(length), variant)
    for checks in ("C0_DEL", "C0_DEL_SPACE")
    for pattern in ("valid", "first_forbidden", "last_forbidden", "high_bytes")
    for length in (0, 8, 16, 31, 32, 64, 128, 512, 4096)
    for variant in ("portable_C_branch", "simdurl")
    if length or pattern == "valid"
}
FORMSCAN_CASES = {
    (mode, pattern, str(length))
    for mode in ("URI", "form")
    for pattern in ("literal", "plus_short", "plus_long", "percent_short", "percent_long", "mixed")
    for length in (16, 64, 128, 512, 4096, 16384)
    if length > 256 or pattern not in ("plus_long", "percent_long")
}


class BenchmarkError(Exception):
    """The benchmark did not produce a complete, usable measurement."""


def positive_number(value, label):
    try:
        number = float(value)
    except (ValueError, TypeError) as error:
        raise BenchmarkError(f"Invalid {label}: {value!r}") from error
    if not math.isfinite(number) or number <= 0:
        raise BenchmarkError(
            f"{label} must be finite and positive, got {value!r}; "
            "increase the iteration count if timing rounded to zero"
        )
    return number


def checksum(line, prefix):
    match = re.fullmatch(re.escape(prefix) + r"([0-9]+)", line)
    if not match:
        raise BenchmarkError("Missing or malformed final checksum")
    return int(match.group(1))


def parse_codec(output, build, iterations):
    lines = output.splitlines()
    backend = {
        "automatic": "Backend: automatic CPU selection (header-only)",
        "scalar": "Backend: portable scalar (header-only)",
    }[build]
    expected_iterations = (
        f"Iterations per case: {iterations}; throughput counts input bytes; CPU time."
    )
    if (len(lines) < 4 or lines[0] != backend
            or lines[1] != expected_iterations
            or lines[2].split() != ["codec", "mode", "pattern", "bytes", "elapsed_ms", "GB/s"]):
        raise BenchmarkError("Unexpected codec header, build, or iteration count")
    result = {}
    seen = set()
    for line in lines[3:-1]:
        fields = line.split()
        if len(fields) != 6:
            raise BenchmarkError(f"Malformed codec row: {line!r}")
        case = tuple(fields[:4])
        if case not in CODEC_CASES or case in seen:
            raise BenchmarkError(f"Unexpected or duplicate codec case: {case!r}")
        seen.add(case)
        elapsed_ms = positive_number(fields[4], "codec elapsed_ms")
        positive_number(fields[5], "codec GB/s")
        operation, mode, pattern, length = case
        name = f"codec/{operation}/{mode}/{pattern}/{length}/simdurl/{build}"
        # GB/s counts bytes, while Bencher's built-in throughput counts ops/s.
        # Use the elapsed CPU time to report ns per complete codec operation.
        result[name] = [positive_number(elapsed_ms * 1e6 / iterations, "codec ns/op")]
    if seen != CODEC_CASES:
        raise BenchmarkError(f"Incomplete codec output: {len(seen)}/{len(CODEC_CASES)} cases")
    return result, checksum(lines[-1], "Checksum: ")


def parse_validation(output, build, iterations):
    lines = output.splitlines()
    backend = {
        "automatic": "# simdurl: automatic CPU selection (header-only)",
        "scalar": "# simdurl: portable C; compiler-generated SIMD remains permitted",
    }[build]
    headers = [
        backend,
        "# comparator: independent portable C branch loop; compiler optimization enabled",
        f"# {iterations} iterations/sample; {VALIDATION_REPEATS} samples/case; CPU time; nanoseconds/operation",
        "# Early rejection may inspect only a prefix; no full-buffer throughput is reported",
        "checks,pattern,bytes,variant,sample,ns_per_op",
    ]
    if len(lines) < 6 or lines[:5] != headers:
        raise BenchmarkError("Unexpected validation header, build, or iteration count")
    result = {}
    seen = set()
    for fields in csv.reader(io.StringIO("\n".join(lines[5:-1]))):
        if len(fields) != 6:
            raise BenchmarkError(f"Malformed validation row: {fields!r}")
        case = tuple(fields[:4])
        sample = fields[4]
        if (case not in VALIDATION_CASES
                or sample not in {str(i) for i in range(1, VALIDATION_REPEATS + 1)}
                or (case, sample) in seen):
            raise BenchmarkError(f"Unexpected or duplicate validation sample: {fields[:5]!r}")
        seen.add((case, sample))
        checks, pattern, length, variant = case
        name = f"validate/{checks}/{pattern}/{length}/{variant}/{build}"
        result.setdefault(name, {})[int(sample)] = positive_number(fields[5], "validation ns/op")
    expected_samples = len(VALIDATION_CASES) * VALIDATION_REPEATS
    if len(seen) != expected_samples:
        raise BenchmarkError(f"Incomplete validation output: {len(seen)}/{expected_samples} samples")
    return {
        name: [samples[i] for i in range(1, VALIDATION_REPEATS + 1)]
        for name, samples in result.items()
    }, checksum(lines[-1], "# checksum: ")


def parse_formscan(output, build, iterations):
    lines = output.splitlines()
    backend = {
        "automatic": "# automatic CPU selection (header-only)",
        "scalar": "# portable C/libc; compiler-generated and libc SIMD remain enabled",
        "compiled": "# compiled library; configured backend selection applies",
    }[build]
    headers = [
        backend,
        "# fixed mode/noinline wrappers; disjoint buffers; CPU ns/operation",
        "# long runs contain 256 literal bytes; short runs contain one",
        "# iterations decrease with input length; five samples after warmup",
        "# input/output barriers, status checks and output sampling are included",
        "mode,pattern,bytes,iterations,sample,ns_per_op,ns_per_byte",
    ]
    if len(lines) < 7 or lines[:6] != headers:
        raise BenchmarkError("Unexpected formscan header or build")
    result = {}
    seen = set()
    try:
        for fields in csv.reader(io.StringIO("\n".join(lines[6:-1])), strict=True):
            if len(fields) != 7:
                raise BenchmarkError(f"Malformed formscan row: {fields!r}")
            case = tuple(fields[:3])
            sample = fields[4]
            if (case not in FORMSCAN_CASES
                    or sample not in {str(i) for i in range(1, FORMSCAN_REPEATS + 1)}
                    or (case, sample) in seen):
                raise BenchmarkError(f"Unexpected or duplicate formscan sample: {fields[:5]!r}")
            mode, pattern, length = case
            expected_iterations = max(1, iterations // ((int(length) + 63) // 64))
            if fields[3] != str(expected_iterations):
                raise BenchmarkError(f"Unexpected formscan iteration count: {fields[:4]!r}")
            ns_per_op = positive_number(fields[5], "formscan ns/op")
            ns_per_byte = positive_number(fields[6], "formscan ns/byte")
            # The C benchmark prints ns/op to three decimals and ns/byte to six.
            tolerance = 0.0005 / int(length) + 0.0000005
            if not math.isclose(ns_per_byte, ns_per_op / int(length), rel_tol=1e-12, abs_tol=tolerance):
                raise BenchmarkError(f"Inconsistent formscan ns/op and ns/byte: {fields!r}")
            seen.add((case, sample))
            name = f"formscan/{mode}/{pattern}/{length}/simdurl/{build}"
            result.setdefault(name, {})[int(sample)] = ns_per_op
    except csv.Error as error:
        raise BenchmarkError(f"Malformed formscan CSV: {error}") from error
    expected_samples = len(FORMSCAN_CASES) * FORMSCAN_REPEATS
    if len(seen) != expected_samples:
        raise BenchmarkError(f"Incomplete formscan output: {len(seen)}/{expected_samples} samples")
    return {
        name: [samples[i] for i in range(1, FORMSCAN_REPEATS + 1)]
        for name, samples in result.items()
    }, checksum(lines[-1], "# checksum: ")


def bmf(samples):
    """The displayed bounds are the observed range, not confidence intervals."""
    return {
        name: {"latency": {
            "value": statistics.median(values),
            "lower_value": min(values),
            "upper_value": max(values),
        }}
        for name, values in sorted(samples.items())
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def machine_metadata():
    metadata = {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "logical_cpus": os.cpu_count(),
    }
    try:
        first_cpu = Path("/proc/cpuinfo").read_text(encoding="utf-8").split("\n\n", 1)[0]
        metadata["cpu"] = {
            key.strip(): value.strip()
            for line in first_cpu.splitlines() if ":" in line
            for key, value in [line.split(":", 1)]
            if key.strip() in {"vendor_id", "model name", "cpu family", "model", "stepping", "flags", "Features"}
        }
    except OSError:
        pass
    if hasattr(os, "sched_getaffinity"):
        metadata["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    return metadata


def run_process(executable, iterations, raw_dir, label, timeout, runs):
    command = [str(executable), str(iterations)]
    record = {"command": command, "label": label}
    runs.append(record)
    start = time.monotonic()
    try:
        process = subprocess.run(
            command, capture_output=True, text=True, encoding="utf-8",
            timeout=timeout, env={**os.environ, "LC_ALL": "C"}, check=False,
        )
    except subprocess.TimeoutExpired as error:
        # TimeoutExpired may return bytes even when text=True.
        for stream in ("stdout", "stderr"):
            value = getattr(error, stream) or b""
            if isinstance(value, bytes):
                value = value.decode("utf-8", errors="replace")
            (raw_dir / f"{label}.{stream}").write_text(value, encoding="utf-8")
        record["timeout"] = True
        raise BenchmarkError(f"{executable.name} exceeded the {timeout}s process timeout") from error
    except OSError as error:
        raise BenchmarkError(f"Cannot execute {executable}: {error}") from error
    finally:
        record["wall_seconds"] = time.monotonic() - start
    record["exit_code"] = process.returncode
    (raw_dir / f"{label}.stdout").write_text(process.stdout, encoding="utf-8")
    (raw_dir / f"{label}.stderr").write_text(process.stderr, encoding="utf-8")
    if process.returncode:
        raise BenchmarkError(
            f"{executable.name} exited with {process.returncode}; see {raw_dir / (label + '.stderr')}"
        )
    return process.stdout


def run(args):
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise BenchmarkError(f"Output directory must be empty: {output_dir}")
    raw_dir = output_dir / "raw"
    raw_dir.mkdir()
    metadata = {
        "harness_version": HARNESS_VERSION,
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": args.commit,
        "machine": machine_metadata(),
        "codec_iterations": args.codec_iterations,
        "codec_repeats": args.codec_repeats,
        "validation_iterations": args.validation_iterations,
        "validation_repeats": VALIDATION_REPEATS,
        "formscan_iterations": args.formscan_iterations,
        "formscan_repeats": FORMSCAN_REPEATS,
        "formscan_iteration_scaling": "max(1, base_iterations // ceil(length / 64))",
        "timer": "process CPU time (C clock)",
        "measure": "latency (nanoseconds per operation)",
        "summary": "median with observed minimum and maximum",
        "backend_selection": "automatic build uses CPU selection; scalar disables explicit SIMD only; compiled uses configured library backend selection",
        "executables": {},
        "runs": [],
        "status": "running",
    }
    samples = {}
    checksums = {}
    try:
        # Alternate builds between process repeats to reduce order bias.
        for family, repeats, iterations, parse, suffix in (
            ("codec", args.codec_repeats, args.codec_iterations, parse_codec, ""),
            ("validate", 1, args.validation_iterations, parse_validation, "_validate"),
            ("formscan", 1, args.formscan_iterations, parse_formscan, "_formscan"),
        ):
            builds = FORMSCAN_BUILDS if family == "formscan" else BUILDS
            for repeat in range(repeats):
                for build in builds[::1 if repeat % 2 == 0 else -1]:
                    executable_name = "simdurl_bench" + suffix
                    if build == "scalar":
                        executable_name += "_portable" if family == "formscan" else "_scalar"
                    elif build == "compiled":
                        executable_name += "_compiled"
                    executable = (args.bin_dir / executable_name).resolve()
                    if executable_name not in metadata["executables"]:
                        metadata["executables"][executable_name] = {
                            "path": str(executable),
                            "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                        }
                    label = f"{family}-{build}-{repeat + 1}"
                    output = run_process(executable, iterations, raw_dir, label, args.timeout, metadata["runs"])
                    parsed, observed_checksum = parse(output, build, iterations)
                    if family in checksums and checksums[family] != observed_checksum:
                        raise BenchmarkError(f"{family} checksums differ between repeated runs or builds")
                    checksums[family] = observed_checksum
                    for name, values in parsed.items():
                        samples.setdefault(name, []).extend(values)
        result = bmf(samples)
        metadata["checksums"] = checksums
        metadata["status"] = "complete"
        write_json(output_dir / "results.json", result)
        return result
    except (BenchmarkError, OSError, ValueError) as error:
        metadata["status"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        metadata["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        write_json(output_dir / "samples.json", samples)
        write_json(output_dir / "metadata.json", metadata)


def positive_int(value):
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=Path("build/benchmarks"))
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-results"), help="empty directory for raw output and metadata")
    parser.add_argument("--codec-iterations", type=positive_int, default=100000)
    parser.add_argument("--codec-repeats", type=positive_int, default=5)
    parser.add_argument("--validation-iterations", type=positive_int, default=100000)
    parser.add_argument("--formscan-iterations", type=positive_int, default=100000,
                        help="base iterations per formscan sample, scaled down above 64 bytes")
    parser.add_argument("--timeout", type=positive_int, default=120, help="timeout in seconds per executable invocation")
    parser.add_argument("--commit", help="source commit SHA to record in metadata")
    args = parser.parse_args(argv)
    try:
        result = run(args)
    except (BenchmarkError, OSError, ValueError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
