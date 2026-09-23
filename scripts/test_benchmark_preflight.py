#!/usr/bin/env python3
"""Check the container's fail-closed VBMI2 gate without requiring VBMI2 hardware."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "benchmark_container", Path(__file__).resolve().parents[1] / "benchmarks/bencher/run.py",
)
container = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(container)

NATIVE_OUTPUT = """SIMDURL_BACKEND operation=encode backend=portable compiled=1 executed=1 skipped=0 cases=20 kernel_calls=10
SIMDURL_BACKEND operation=encode backend=vbmi2 compiled=1 executed=1 skipped=0 cases=20 kernel_calls=10
SIMDURL_BACKEND operation=decode backend=vbmi2 compiled=1 executed=1 skipped=0 cases=30 kernel_calls=15
100 direct backend checks passed
"""
DISPATCH = {"compiler": "14.3.0", "encode": "vbmi2", "decode": "vbmi2",
            "validate": "avx2", "ascii_lower": "avx2", "hex_encode": "avx2"}


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="simdurl-preflight-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.binaries = self.root / "benchmarks"
        self.binaries.mkdir()
        (self.binaries / "source-sha").write_text("a" * 40 + "\n")
        (self.binaries / "compiler-flags").write_text("-O3 -DNDEBUG\n")
        (self.binaries / "test-compiler-flags").write_text("-O3 -UNDEBUG\n")
        self.backend()
        self.native()
        self.benchmark()

    def executable(self, name, body):
        path = self.binaries / name
        path.write_text("#!/usr/bin/env python3\nimport sys\nfrom pathlib import Path\n" + body)
        path.chmod(0o755)

    def backend(self, dispatch=None, stdout=None, status=0):
        if stdout is None:
            stdout = json.dumps(DISPATCH if dispatch is None else dispatch)
        self.executable("backend_info", f"print({stdout!r})\nsys.exit({status})\n")

    def native(self, stdout=NATIVE_OUTPUT, status=0):
        self.executable(
            "simdurl_test_backends",
            "assert sys.argv[1:] == ['--require=vbmi2']\n"
            "Path(__file__).with_name('native-ran').touch()\n"
            f"print({stdout!r}, end='')\n"
            f"print('native test diagnostics', file=sys.stderr)\nsys.exit({status})\n",
        )

    def benchmark(self, status=0):
        (self.root / "benchmark.py").write_text(
            "import json, sys\nfrom pathlib import Path\n"
            "Path(__file__).with_name('benchmark-ran').touch()\n"
            "output = Path(sys.argv[sys.argv.index('--output-dir') + 1])\n"
            "assert not list(output.iterdir())\n"
            "(output / 'samples.json').write_text('{\"samples\":[1,2,3]}')\n"
            "print('{\"metric\":{\"latency\":{\"value\":1}}}')\n"
            "print('measurement diagnostics', file=sys.stderr)\n"
            f"sys.exit({status})\n"
        )

    def run_container(self, requirement="1"):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, {"SIMDURL_BENCH_REQUIRE_VBMI2": requirement}), \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = container.main([], self.root)
        envelopes = [json.loads(line)["simdurl_evidence"] for line in stderr.getvalue().splitlines()
                     if line.startswith('{"simdurl_evidence":')]
        self.assertEqual(len(envelopes), 1, stderr.getvalue())
        self.assertEqual(envelopes[0]["exit_code"], status)
        return status, stdout.getvalue(), envelopes[0]

    def assert_preflight_failure(self, **kwargs):
        status, metrics, evidence = self.run_container(**kwargs)
        self.assertNotEqual(status, 0)
        self.assertEqual(metrics, "")
        self.assertFalse((self.root / "benchmark-ran").exists())
        self.assertEqual(evidence["preflight"]["status"], "failed")
        self.assertIn("error", evidence["preflight"])
        return evidence

    def test_native_vbmi2_execution_allows_avx2_helpers_and_metrics(self):
        status, metrics, evidence = self.run_container()
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(metrics)["metric"]["latency"]["value"], 1)
        self.assertTrue((self.binaries / "native-ran").exists())
        self.assertEqual(evidence["dispatch"], DISPATCH)
        self.assertEqual(evidence["preflight"]["status"], "passed")
        native = evidence["preflight"]["native_tests"]
        self.assertEqual(native["command"][-1], "--require=vbmi2")
        self.assertEqual(native["exit_code"], 0)
        self.assertEqual(native["stdout"], NATIVE_OUTPUT)
        self.assertEqual(len(native["records"]), 3)
        self.assertIn("native test diagnostics", native["stderr"])
        self.assertIn("samples.json", evidence["files"])
        self.assertIn("-UNDEBUG", evidence["native_test_compiler_flags"])

    def test_each_codec_requires_vbmi2_dispatch(self):
        for operation in ("encode", "decode"):
            with self.subTest(operation=operation):
                self.backend(dispatch={**DISPATCH, operation: "portable"})
                evidence = self.assert_preflight_failure()
                self.assertNotIn("native_tests", evidence["preflight"])
                self.assertFalse((self.binaries / "native-ran").exists())

    def test_missing_dispatch_field_fails_closed(self):
        self.backend(dispatch={"encode": "vbmi2"})
        self.assert_preflight_failure()

    def test_backend_failure_preserves_probe_output(self):
        self.backend(stdout="backend probe failed", status=4)
        evidence = self.assert_preflight_failure()
        probe = evidence["preflight"]["backend_probe"]
        self.assertEqual(probe["exit_code"], 4)
        self.assertEqual(probe["stdout"], "backend probe failed\n")

    def test_malformed_backend_json_fails_closed(self):
        for stdout in ("not-json", "[]", "null"):
            with self.subTest(stdout=stdout):
                self.backend(stdout=stdout)
                self.assert_preflight_failure()

    def test_missing_backend_executable_retains_evidence(self):
        (self.binaries / "backend_info").unlink()
        evidence = self.assert_preflight_failure()
        self.assertIn("error", evidence["preflight"]["backend_probe"])

    def test_native_failure_retains_stdout_and_stderr(self):
        self.native(status=7)
        evidence = self.assert_preflight_failure()
        native = evidence["preflight"]["native_tests"]
        self.assertEqual(native["exit_code"], 7)
        self.assertEqual(native["stdout"], NATIVE_OUTPUT)
        self.assertIn("native test diagnostics", native["stderr"])

    def test_missing_native_executable_fails_closed(self):
        (self.binaries / "simdurl_test_backends").unlink()
        evidence = self.assert_preflight_failure()
        self.assertIn("error", evidence["preflight"]["native_tests"])

    def test_native_exit_zero_without_execution_evidence_fails(self):
        for stdout in ("all tests passed\n", NATIVE_OUTPUT.replace("kernel_calls=15", "kernel_calls=0"),
                       NATIVE_OUTPUT.replace("executed=1", "executed=0"),
                       NATIVE_OUTPUT.replace("cases=30", "cases=invalid"),
                       NATIVE_OUTPUT.replace("operation=decode", "operation=encode")):
            with self.subTest(stdout=stdout):
                self.native(stdout=stdout)
                self.assert_preflight_failure()

    def test_native_timeout_retains_partial_output(self):
        probe = subprocess.CompletedProcess([], 0, json.dumps(DISPATCH), "")
        timeout = subprocess.TimeoutExpired([], 180, output=b"partial native result", stderr=b"details")
        with mock.patch.object(container.subprocess, "run", side_effect=[probe, timeout]):
            evidence = self.assert_preflight_failure()
        native = evidence["preflight"]["native_tests"]
        self.assertTrue(native["timeout"])
        self.assertEqual(native["stdout"], "partial native result")
        self.assertEqual(native["stderr"], "details")

    def test_invalid_requirement_fails_closed(self):
        self.assert_preflight_failure(requirement="true")

    def test_optional_gate_keeps_existing_non_vbmi2_benchmark_supported(self):
        self.backend(dispatch={**DISPATCH, "encode": "avx2", "decode": "portable"})
        (self.binaries / "simdurl_test_backends").unlink()
        status, metrics, evidence = self.run_container(requirement="0")
        self.assertEqual(status, 0)
        self.assertTrue(metrics)
        self.assertFalse(evidence["preflight"]["require_vbmi2"])
        self.assertNotIn("native_tests", evidence["preflight"])

    def test_measurement_failure_preserves_samples_without_publishing_metrics(self):
        self.benchmark(status=3)
        status, metrics, evidence = self.run_container()
        self.assertEqual(status, 3)
        self.assertEqual(metrics, "")
        self.assertEqual(evidence["preflight"]["status"], "passed")
        self.assertIn("samples.json", evidence["files"])
        self.assertIn("metric", evidence["files"]["benchmark.stdout"])
        self.assertIn("measurement diagnostics", evidence["files"]["benchmark.stderr"])


if __name__ == "__main__":
    unittest.main()
