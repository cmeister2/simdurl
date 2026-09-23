#!/usr/bin/env python3
"""Emit metrics on stdout and preserve measurement evidence in job stderr."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


class PreflightError(Exception):
    pass


def captured_text(value):
    # TimeoutExpired can carry bytes even when subprocess.run uses text=True.
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def run_preflight(command, record, timeout):
    record["command"] = [str(argument) for argument in command]
    try:
        process = subprocess.run(
            record["command"], capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        record.update(timeout=True, stdout=captured_text(error.stdout),
                      stderr=captured_text(error.stderr))
        raise PreflightError(f"{command[0]} exceeded the {timeout}s preflight timeout") from error
    except OSError as error:
        record["error"] = str(error)
        raise PreflightError(f"Cannot execute {command[0]}: {error}") from error
    record.update(exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr)
    if process.returncode:
        raise PreflightError(f"{command[0]} exited with {process.returncode}")
    return process.stdout


def require_native_execution(stdout):
    """Require positive execution evidence for both direct VBMI2 kernels."""
    records = []
    for line in stdout.splitlines():
        if not line.startswith("SIMDURL_BACKEND "):
            continue
        fields = [field.split("=", 1) for field in line.split()[1:]]
        if any(len(field) != 2 for field in fields):
            raise PreflightError("Malformed native backend execution evidence")
        record = dict(fields)
        if len(record) != len(fields):
            raise PreflightError("Duplicate fields in native backend execution evidence")
        records.append(record)
    for operation in ("encode", "decode"):
        matches = [record for record in records if
                   record.get("operation") == operation and record.get("backend") == "vbmi2"]
        if len(matches) != 1:
            raise PreflightError(f"Expected one native VBMI2 {operation} execution record")
        record = matches[0]
        if any(record.get(field) != expected for field, expected in
               (("compiled", "1"), ("executed", "1"), ("skipped", "0"))):
            raise PreflightError(f"Native VBMI2 {operation} did not execute")
        try:
            positive_counts = all(int(record[field]) > 0 for field in ("cases", "kernel_calls"))
        except (KeyError, ValueError):
            positive_counts = False
        if not positive_counts:
            raise PreflightError(f"Native VBMI2 {operation} has no positive case/kernel counts")
    return records


def main(argv=None, root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parent
    requirement = os.environ.get("SIMDURL_BENCH_REQUIRE_VBMI2", "0")
    preflight = {"require_vbmi2": requirement == "1", "status": "running"}
    evidence = {"schema_version": 2, "preflight": preflight, "files": {}}
    status = 1
    with tempfile.TemporaryDirectory(prefix="simdurl-benchmark-") as directory:
        output = Path(directory)
        try:
            if requirement not in ("0", "1"):
                raise PreflightError("SIMDURL_BENCH_REQUIRE_VBMI2 must be 0 or 1")
            evidence["source_sha"] = (root / "benchmarks/source-sha").read_text().strip()
            evidence["compiler_flags"] = (root / "benchmarks/compiler-flags").read_text().strip()
            preflight["backend_probe"] = {}
            backend = json.loads(run_preflight(
                [root / "benchmarks/backend_info"], preflight["backend_probe"], timeout=10,
            ))
            if not isinstance(backend, dict):
                raise PreflightError("Backend probe must produce a JSON object")
            evidence["dispatch"] = backend
            if preflight["require_vbmi2"]:
                if any(backend.get(operation) != "vbmi2" for operation in ("encode", "decode")):
                    raise PreflightError("VBMI2 is required for both encode and decode dispatch")
                evidence["native_test_compiler_flags"] = (
                    root / "benchmarks/test-compiler-flags"
                ).read_text().strip()
                preflight["native_tests"] = {}
                stdout = run_preflight(
                    [root / "benchmarks/simdurl_test_backends", "--require=vbmi2"],
                    preflight["native_tests"], timeout=180,
                )
                preflight["native_tests"]["records"] = require_native_execution(stdout)
            preflight["status"] = "passed"
            command = [
                sys.executable, str(root / "benchmark.py"),
                "--bin-dir", str(root / "benchmarks"),
                "--output-dir", str(output),
                "--commit", evidence["source_sha"],
            ]
            # Extra arguments allow smoke-testing the identical image locally.
            command.extend(sys.argv[1:] if argv is None else argv)
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", check=False)
            (output / "benchmark.stdout").write_text(result.stdout, encoding="utf-8")
            (output / "benchmark.stderr").write_text(result.stderr, encoding="utf-8")
            status = result.returncode
            sys.stderr.write(result.stderr)
            # Nothing on stdout is publishable until every prerequisite succeeds.
            if status == 0:
                sys.stdout.write(result.stdout)
        except (PreflightError, OSError, ValueError) as error:
            evidence["error"] = str(error)
            if preflight["status"] == "running":
                preflight.update(status="failed", error=str(error))
            print(f"Benchmark failed: {error}", file=sys.stderr)
        finally:
            evidence["exit_code"] = status
            evidence["files"] = {
                str(path.relative_to(output)): path.read_text(encoding="utf-8", errors="replace")
                for path in sorted(output.rglob("*")) if path.is_file()
            }
            # The remote filesystem is ephemeral. Always retain diagnostics in
            # job output, including when dispatch discovery or native tests fail.
            print(json.dumps({"simdurl_evidence": evidence}, separators=(",", ":")), file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
