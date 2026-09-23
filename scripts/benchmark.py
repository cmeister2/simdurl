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


HARNESS_VERSION = 5
CALIBRATED_POLICY = {
    "mode": "calibrated",
    "samples_per_case": 20,
    "min_sample_ms": 50,
    "target_sample_ms": 100,
    "warmup_ms": 100,
}
VALIDATION_REPEATS = 5
FORMSCAN_REPEATS = 5
HELPER_REPEATS = 5
BUILDS = ("automatic", "scalar")
FORMSCAN_BUILDS = ("automatic", "scalar", "compiled")
HELPER_BUILDS = ("automatic", "scalar", "compiled")
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

HELPER_CASES = {
    (operation, pattern, str(length), "runtime", variant)
    for operation in ("ascii_copy", "ascii_inplace_already_lowered", "hex_lower", "hex_upper")
    for pattern in (("mixed_ascii", "unchanged_ascii", "high_bytes")
                    if operation.startswith("ascii_") else ("binary",))
    for length in (0, 1, 8, 15, 16, 17, 20, 31, 32, 33, 48, 63, 64, 65, 128, 512, 4096)
    for variant in ("portable_C_comparator", "simdurl")
    if length or pattern in ("mixed_ascii", "binary")
} | {
    (operation, "binary", str(length), "fixed", variant)
    for operation in ("hex_lower", "hex_upper")
    for length in (16, 20, 32, 64)
    for variant in ("portable_C_comparator", "simdurl")
}


# Ordinary calls plus workloads that expose SIMD throughput. Keep the existing
# names and workload semantics for historical comparison; --suite full retains
# the complete diagnostic matrices above.
CORE_CASES = {
    "codec": {
        ("encode", "URI", "mixed", "128"),
        ("decode", "form", "mixed", "128"),
        ("encode", "URI", "literal", "4096"),
        ("encode", "URI", "dense", "4096"),
        ("decode", "URI", "dense", "4096"),
    },
    "validate": {("C0_DEL_SPACE", "valid", "4096", "simdurl")},
    "formscan": {("form", "plus_long", "16384")},
    "helpers": {
        ("ascii_copy", "mixed_ascii", "128", "runtime", "simdurl"),
        ("hex_lower", "binary", "32", "fixed", "simdurl"),
        ("hex_lower", "binary", "4096", "runtime", "simdurl"),
    },
}


class BenchmarkError(Exception):
    """The benchmark did not produce a complete, usable measurement."""


def core_names(family=None):
    names = set()
    for group, cases in CORE_CASES.items():
        if family is not None and family != group:
            continue
        for case in cases:
            parts = (group, *case)
            if group in ("codec", "formscan"):
                parts += ("simdurl",)
            names.add("/".join((*parts, "automatic")))
    return names


def validate_calibrated_batches(batches, expected_names=None):
    """Reconstruct scores only from complete, sufficiently long measurements.

    Shared with the Azure evidence gate so metadata cannot claim a different
    sampling policy from the batches that actually produced the scores.
    """
    expected = core_names() if expected_names is None else expected_names
    if not isinstance(batches, dict) or set(batches) != expected:
        raise BenchmarkError("Calibrated benchmark case set does not match the core suite")
    samples = {}
    for name, records in batches.items():
        if not isinstance(records, list):
            raise BenchmarkError(f"Invalid calibrated batches for {name}")
        values = []
        warmup_ns = 0
        calibrated = False
        stage = "warmup"
        for record in records:
            if len(values) == CALIBRATED_POLICY["samples_per_case"]:
                raise BenchmarkError(f"Unexpected batches after the final sample for {name}")
            if (not isinstance(record, dict)
                    or set(record) != {"phase", "sample", "iterations", "elapsed_ns"}
                    or any(type(record.get(field)) is not int for field in
                           ("sample", "iterations", "elapsed_ns"))
                    or record["iterations"] <= 0 or record["elapsed_ns"] < 0):
                raise BenchmarkError(f"Invalid calibrated batch for {name}")
            phase, index, elapsed = record["phase"], record["sample"], record["elapsed_ns"]
            if phase not in ("warmup", "calibration", "sample", "retry"):
                raise BenchmarkError(f"Unknown calibrated batch phase for {name}")
            if phase != "sample" and index != 0:
                raise BenchmarkError(f"Discarded batches must have sample index zero for {name}")
            if phase == "warmup":
                if stage != "warmup":
                    raise BenchmarkError(f"Warmup must precede calibration and sampling for {name}")
                warmup_ns += elapsed
                continue
            if warmup_ns < CALIBRATED_POLICY["warmup_ms"] * 1_000_000:
                raise BenchmarkError(f"Insufficient discarded warmup for {name}")
            if phase == "calibration":
                stage = "calibration"
                calibrated = elapsed >= CALIBRATED_POLICY["target_sample_ms"] * 1_000_000
                continue
            if not calibrated:
                raise BenchmarkError(f"Missing completed calibration for {name}")
            stage = "sample"
            if phase == "retry":
                if elapsed >= CALIBRATED_POLICY["min_sample_ms"] * 1_000_000:
                    raise BenchmarkError(f"Only short samples may be retried for {name}")
                continue
            if index != len(values) + 1:
                raise BenchmarkError(f"Duplicate or out-of-order calibrated sample for {name}")
            if elapsed < CALIBRATED_POLICY["min_sample_ms"] * 1_000_000:
                raise BenchmarkError(f"Calibrated sample is too short for {name}")
            values.append(positive_number(elapsed / record["iterations"], "calibrated ns/op"))
        if len(values) != CALIBRATED_POLICY["samples_per_case"] or stage != "sample":
            raise BenchmarkError(f"Incomplete calibrated samples for {name}")
        samples[name] = values
    return samples


def parse_calibrated(output, family):
    lines = output.splitlines()
    if lines[:2] != ["# simdurl calibrated v1", "benchmark,phase,sample,iterations,elapsed_ns"]:
        raise BenchmarkError("Unexpected calibrated benchmark header")
    batches = {}
    try:
        for fields in csv.reader(io.StringIO("\n".join(lines[2:-1])), strict=True):
            if len(fields) != 5 or any(not re.fullmatch(r"[0-9]+", v) for v in fields[2:]):
                raise BenchmarkError(f"Malformed calibrated benchmark row: {fields!r}")
            name, phase, sample, iterations, elapsed_ns = fields
            batches.setdefault(name, []).append({
                "phase": phase, "sample": int(sample), "iterations": int(iterations),
                "elapsed_ns": int(elapsed_ns),
            })
    except csv.Error as error:
        raise BenchmarkError(f"Malformed calibrated CSV: {error}") from error
    samples = validate_calibrated_batches(batches, core_names(family))
    return samples, checksum(lines[-1], "# checksum: "), batches


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


def parse_codec(output, build, iterations, suite="full"):
    cases = CORE_CASES["codec"] if suite == "core" else CODEC_CASES
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
        if case not in cases or case in seen:
            raise BenchmarkError(f"Unexpected or duplicate codec case: {case!r}")
        seen.add(case)
        elapsed_ms = positive_number(fields[4], "codec elapsed_ms")
        positive_number(fields[5], "codec GB/s")
        operation, mode, pattern, length = case
        name = f"codec/{operation}/{mode}/{pattern}/{length}/simdurl/{build}"
        # GB/s counts bytes, while Bencher's built-in throughput counts ops/s.
        # Use the elapsed CPU time to report ns per complete codec operation.
        result[name] = [positive_number(elapsed_ms * 1e6 / iterations, "codec ns/op")]
    if seen != cases:
        raise BenchmarkError(f"Incomplete codec output: {len(seen)}/{len(cases)} cases")
    return result, checksum(lines[-1], "Checksum: ")


def parse_validation(output, build, iterations, suite="full"):
    cases = CORE_CASES["validate"] if suite == "core" else VALIDATION_CASES
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
        if (case not in cases
                or sample not in {str(i) for i in range(1, VALIDATION_REPEATS + 1)}
                or (case, sample) in seen):
            raise BenchmarkError(f"Unexpected or duplicate validation sample: {fields[:5]!r}")
        seen.add((case, sample))
        checks, pattern, length, variant = case
        name = f"validate/{checks}/{pattern}/{length}/{variant}/{build}"
        result.setdefault(name, {})[int(sample)] = positive_number(fields[5], "validation ns/op")
    expected_samples = len(cases) * VALIDATION_REPEATS
    if len(seen) != expected_samples:
        raise BenchmarkError(f"Incomplete validation output: {len(seen)}/{expected_samples} samples")
    return {
        name: [samples[i] for i in range(1, VALIDATION_REPEATS + 1)]
        for name, samples in result.items()
    }, checksum(lines[-1], "# checksum: ")


def parse_formscan(output, build, iterations, suite="full"):
    cases = CORE_CASES["formscan"] if suite == "core" else FORMSCAN_CASES
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
            if (case not in cases
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
    expected_samples = len(cases) * FORMSCAN_REPEATS
    if len(seen) != expected_samples:
        raise BenchmarkError(f"Incomplete formscan output: {len(seen)}/{expected_samples} samples")
    return {
        name: [samples[i] for i in range(1, FORMSCAN_REPEATS + 1)]
        for name, samples in result.items()
    }, checksum(lines[-1], "# checksum: ")


def parse_helpers(output, build, iterations, suite="full"):
    cases = CORE_CASES["helpers"] if suite == "core" else HELPER_CASES
    lines = output.splitlines()
    backend = {
        "automatic": "# simdurl: automatic CPU selection (header-only)",
        "scalar": "# simdurl: portable C header-only; compiler-generated SIMD permitted",
        "compiled": "# simdurl: compiled library; configured library CPU selection applies",
    }[build]
    headers = [
        backend,
        "# comparators: independent portable C; optimization and vectorization enabled",
        "# comparators assume valid arguments; simdurl includes API argument checks",
        "# both variants use noinline wrappers; compiled simdurl retains a separate API boundary",
        "# fixed hex wrappers expose constant lengths and case to both variants",
        "# inplace inputs are already lowercased before timing; no input-reset cost included",
        "# checksums, output sampling and call overhead are included in both timings",
        f"# {iterations} iterations/sample; {HELPER_REPEATS} "
        f"{'samples' if suite == 'core' else 'alternating samples'}; CPU time; ns/operation",
        "operation,pattern,bytes,length_kind,variant,sample,ns_per_op",
    ]
    if len(lines) < 10 or lines[:9] != headers:
        raise BenchmarkError("Unexpected helpers header, build, or iteration count")
    result = {}
    seen = set()
    try:
        for fields in csv.reader(io.StringIO("\n".join(lines[9:-1])), strict=True):
            if len(fields) != 7:
                raise BenchmarkError(f"Malformed helpers row: {fields!r}")
            case = tuple(fields[:5])
            sample = fields[5]
            if (case not in cases
                    or sample not in {str(i) for i in range(1, HELPER_REPEATS + 1)}
                    or (case, sample) in seen):
                raise BenchmarkError(f"Unexpected or duplicate helpers sample: {fields[:6]!r}")
            seen.add((case, sample))
            operation, pattern, length, length_kind, variant = case
            name = f"helpers/{operation}/{pattern}/{length}/{length_kind}/{variant}/{build}"
            result.setdefault(name, {})[int(sample)] = positive_number(fields[6], "helpers ns/op")
    except csv.Error as error:
        raise BenchmarkError(f"Malformed helpers CSV: {error}") from error
    expected_samples = len(cases) * HELPER_REPEATS
    if len(seen) != expected_samples:
        raise BenchmarkError(f"Incomplete helpers output: {len(seen)}/{expected_samples} samples")
    return {
        name: [samples[i] for i in range(1, HELPER_REPEATS + 1)]
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


def run_process(executable, iterations, raw_dir, label, timeout, runs, suite="full", sampling="fixed"):
    command = [str(executable), str(iterations)]
    if sampling == "calibrated":
        command.append("--calibrated")
    elif suite == "core":
        command.append("--core")
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
    calibrated = args.sampling == "calibrated"
    repeats = CALIBRATED_POLICY["samples_per_case"] if calibrated else 5
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise BenchmarkError(f"Output directory must be empty: {output_dir}")
    raw_dir = output_dir / "raw"
    raw_dir.mkdir()
    metadata = {
        "harness_version": HARNESS_VERSION,
        "suite": args.suite,
        "sampling_policy": dict(CALIBRATED_POLICY) if calibrated else {"mode": "fixed"},
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "commit": args.commit,
        "machine": machine_metadata(),
        "codec_iterations": args.codec_iterations,
        "codec_repeats": args.codec_repeats,
        "validation_iterations": args.validation_iterations,
        "validation_repeats": repeats,
        "formscan_iterations": args.formscan_iterations,
        "formscan_repeats": repeats,
        "formscan_iteration_scaling": "max(1, base_iterations // ceil(length / 64))" +
            (" for initial seed only; then calibrated per case" if calibrated else ""),
        "helper_iterations": args.helper_iterations,
        "helper_repeats": repeats,
        "iteration_policy": "initial seeds; actual counts recorded in batches" if calibrated else "fixed",
        "timer": "process CPU time (C clock)",
        "measure": "latency (nanoseconds per operation)",
        "summary": "median with observed minimum and maximum",
        "backend_selection": "automatic build uses CPU selection; scalar disables explicit SIMD only; compiled uses configured library backend selection",
        "executables": {},
        "runs": [],
        "status": "running",
    }
    if calibrated:
        metadata["batches"] = {}
    samples = {}
    checksums = {}
    try:
        # Alternate builds between process repeats to reduce order bias.
        for family, repeats, iterations, parse, suffix in (
            ("codec", 1 if calibrated else args.codec_repeats, args.codec_iterations, parse_codec, ""),
            ("validate", 1, args.validation_iterations, parse_validation, "_validate"),
            ("formscan", 1, args.formscan_iterations, parse_formscan, "_formscan"),
            ("helpers", 1, args.helper_iterations, parse_helpers, "_helpers"),
        ):
            builds = (FORMSCAN_BUILDS if family == "formscan" else
                      HELPER_BUILDS if family == "helpers" else BUILDS)
            if args.suite == "core":
                builds = ("automatic",)
            for repeat in range(repeats):
                for build in builds[::1 if repeat % 2 == 0 else -1]:
                    executable_name = "simdurl_bench" + suffix
                    if build == "scalar":
                        executable_name += "_portable" if family in ("formscan", "helpers") else "_scalar"
                    elif build == "compiled":
                        executable_name += "_compiled"
                    executable = (args.bin_dir / executable_name).resolve()
                    if executable_name not in metadata["executables"]:
                        metadata["executables"][executable_name] = {
                            "path": str(executable),
                            "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                        }
                    label = f"{family}-{build}-{repeat + 1}"
                    output = run_process(executable, iterations, raw_dir, label, args.timeout,
                                         metadata["runs"], args.suite, args.sampling)
                    if calibrated:
                        parsed, observed_checksum, batches = parse_calibrated(output, family)
                        metadata["batches"].update(batches)
                    else:
                        parsed, observed_checksum = parse(output, build, iterations, args.suite)
                    if family in checksums and checksums[family] != observed_checksum:
                        raise BenchmarkError(f"{family} checksums differ between repeated runs or builds")
                    checksums[family] = observed_checksum
                    for name, values in parsed.items():
                        samples.setdefault(name, []).extend(values)
        result = bmf(samples)
        metadata["checksums"] = checksums
        metadata["benchmark_count"] = len(result)
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
    parser.add_argument("--suite", choices=("core", "full"), default="core",
                        help="core: ten regression benchmarks (default); full: diagnostic matrix")
    parser.add_argument("--sampling", choices=("calibrated", "fixed"),
                        help="default: calibrated for core, fixed for full; fixed also supports quick smoke tests")
    parser.add_argument("--bin-dir", type=Path, default=Path("build/benchmarks"))
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-results"), help="empty directory for raw output and metadata")
    parser.add_argument("--codec-iterations", type=positive_int, default=100000,
                        help="fixed iterations, or initial calibration seed")
    parser.add_argument("--codec-repeats", type=positive_int,
                        help="fixed sampling only (default: 5); calibrated sampling always collects 20")
    parser.add_argument("--validation-iterations", type=positive_int, default=100000)
    parser.add_argument("--formscan-iterations", type=positive_int, default=100000,
                        help="base iterations per formscan sample, scaled down above 64 bytes")
    parser.add_argument("--helper-iterations", type=positive_int, default=500000,
                        help="iterations per ASCII/hex helper sample")
    parser.add_argument("--timeout", type=positive_int, default=120, help="timeout in seconds per executable invocation")
    parser.add_argument("--commit", help="source commit SHA to record in metadata")
    args = parser.parse_args(argv)
    args.sampling = args.sampling or ("calibrated" if args.suite == "core" else "fixed")
    if args.sampling == "calibrated":
        if args.suite != "core":
            parser.error("calibrated sampling requires --suite core")
        if args.codec_repeats is not None:
            parser.error("--codec-repeats requires --sampling fixed")
        args.codec_repeats = CALIBRATED_POLICY["samples_per_case"]
    elif args.codec_repeats is None:
        args.codec_repeats = 5
    try:
        result = run(args)
    except (BenchmarkError, OSError, ValueError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
