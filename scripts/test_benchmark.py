#!/usr/bin/env python3
"""Focused validation of benchmark import and failure handling (stdlib only)."""

import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import benchmark


def codec_output(build="automatic", iterations=10000, elapsed="2.500"):
    backend = ("automatic CPU selection" if build == "automatic" else "portable scalar")
    lines = [
        f"Backend: {backend} (header-only)",
        f"Iterations per case: {iterations}; throughput counts input bytes; CPU time.",
        "codec  mode pattern bytes   elapsed_ms       GB/s",
    ]
    for length in (16, 128, 4096):
        for pattern in ("literal", "mixed", "dense"):
            for mode in ("URI", "form"):
                for operation in ("encode", "decode"):
                    lines.append(f"{operation} {mode} {pattern} {length} {elapsed} 1.250")
    lines.append("Checksum: 123456")
    return "\n".join(lines) + "\n"


def validation_output(build="automatic", iterations=100000):
    backend = ("automatic CPU selection (header-only)" if build == "automatic"
               else "portable C; compiler-generated SIMD remains permitted")
    lines = [
        f"# simdurl: {backend}",
        "# comparator: independent portable C branch loop; compiler optimization enabled",
        f"# {iterations} iterations/sample; 5 samples/case; CPU time; nanoseconds/operation",
        "# Early rejection may inspect only a prefix; no full-buffer throughput is reported",
        "checks,pattern,bytes,variant,sample,ns_per_op",
    ]
    for length in (0, 8, 16, 31, 32, 64, 128, 512, 4096):
        for checks in ("C0_DEL", "C0_DEL_SPACE"):
            for pattern in ("valid", "first_forbidden", "last_forbidden", "high_bytes"):
                if length == 0 and pattern != "valid":
                    continue
                for sample in range(1, 6):
                    variants = ("portable_C_branch", "simdurl")
                    # The real harness alternates the variant measurement order.
                    for variant in variants[::1 if sample % 2 else -1]:
                        lines.append(f"{checks},{pattern},{length},{variant},{sample},{sample + 0.5:.3f}")
    lines.append("# checksum: 789012")
    return "\n".join(lines) + "\n"


def formscan_output(build="automatic", iterations=100000):
    backend = {
        "automatic": "automatic CPU selection (header-only)",
        "scalar": "portable C/libc; compiler-generated and libc SIMD remain enabled",
        "compiled": "compiled library; configured backend selection applies",
    }[build]
    lines = [
        f"# {backend}",
        "# fixed mode/noinline wrappers; disjoint buffers; CPU ns/operation",
        "# long runs contain 256 literal bytes; short runs contain one",
        "# iterations decrease with input length; five samples after warmup",
        "# input/output barriers, status checks and output sampling are included",
        "mode,pattern,bytes,iterations,sample,ns_per_op,ns_per_byte",
    ]
    for length in (16, 64, 128, 512, 4096, 16384):
        for pattern in ("literal", "plus_short", "plus_long", "percent_short", "percent_long", "mixed"):
            if length <= 256 and pattern in ("plus_long", "percent_long"):
                continue
            for mode in ("URI", "form"):
                count = max(1, iterations // ((length + 63) // 64))
                # Sample identity must survive output reordering.
                for sample in (1, 3, 5, 2, 4):
                    timing = sample + 0.5
                    lines.append(f"{mode},{pattern},{length},{count},{sample},{timing:.3f},{timing / length:.6f}")
    lines.append("# checksum: 345678")
    return "\n".join(lines) + "\n"


class ParserTests(unittest.TestCase):
    def test_codec_converts_milliseconds_to_ns_per_operation(self):
        samples, checksum = benchmark.parse_codec(codec_output(), "automatic", 10000)
        self.assertEqual(checksum, 123456)
        self.assertEqual(len(samples), 36)
        self.assertEqual(samples["codec/encode/URI/literal/16/simdurl/automatic"], [250.0])

    def test_validation_retains_five_samples_and_empty_input(self):
        samples, checksum = benchmark.parse_validation(validation_output(), "automatic", 100000)
        self.assertEqual(checksum, 789012)
        self.assertEqual(len(samples), 132)
        self.assertEqual(samples["validate/C0_DEL/valid/0/simdurl/automatic"], [1.5, 2.5, 3.5, 4.5, 5.5])
        portable, _ = benchmark.parse_validation(validation_output("scalar"), "scalar", 100000)
        self.assertEqual(samples.keys() & portable.keys(), set())

    def test_formscan_retains_five_ordered_samples_and_sixty_cases_per_build(self):
        automatic, checksum = benchmark.parse_formscan(formscan_output(), "automatic", 100000)
        scalar, _ = benchmark.parse_formscan(formscan_output("scalar"), "scalar", 100000)
        compiled, _ = benchmark.parse_formscan(formscan_output("compiled"), "compiled", 100000)
        self.assertEqual(checksum, 345678)
        self.assertEqual((len(automatic), len(scalar), len(compiled)), (60, 60, 60))
        self.assertEqual(len(automatic.keys() | scalar.keys() | compiled.keys()), 180)
        self.assertEqual(automatic["formscan/form/plus_long/16384/simdurl/automatic"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertEqual(compiled["formscan/form/plus_long/16384/simdurl/compiled"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertNotIn("formscan/form/plus_long/128/simdurl/automatic", automatic)

    def test_formscan_rejects_headers_from_other_build_variants(self):
        for expected in ("automatic", "scalar", "compiled"):
            for actual in ("automatic", "scalar", "compiled"):
                if actual != expected:
                    with self.subTest(expected=expected, actual=actual), self.assertRaisesRegex(
                            benchmark.BenchmarkError, "header or build"):
                        benchmark.parse_formscan(formscan_output(actual), expected, 100000)

    def test_formscan_checks_scaled_iteration_floor_and_minimum(self):
        for iterations in (1, 100003):
            with self.subTest(iterations=iterations):
                output = formscan_output(iterations=iterations)
                samples, _ = benchmark.parse_formscan(output, "automatic", iterations)
                self.assertEqual(len(samples), 60)
                malformed = output.replace("URI,literal,16384,", "URI,literal,16384,0", 1)
                with self.assertRaisesRegex(benchmark.BenchmarkError, "iteration count"):
                    benchmark.parse_formscan(malformed, "automatic", iterations)

    def test_formscan_rejects_changed_matrix_samples_headers_and_checksum(self):
        lines = formscan_output().splitlines()
        malformed = {
            "missing sample": lines[:6] + lines[7:],
            "duplicate sample": lines[:6] + [lines[6]] + lines[6:],
            "unknown case": lines[:6] + [lines[6].replace("literal", "other")] + lines[7:],
            "invalid sample": lines[:6] + [lines[6].replace(",1,", ",6,")] + lines[7:],
            "wrong iteration count": lines[:6] + [lines[6].replace(",100000,", ",99999,")] + lines[7:],
            "missing field": lines[:6] + [lines[6].rsplit(",", 1)[0]] + lines[7:],
            "invalid CSV": lines[:6] + ['"' + lines[6]] + lines[7:],
            "compiled backend": ["# compiled library; configured backend selection applies"] + lines[1:],
            "changed header": lines[:2] + ["# different workload"] + lines[3:],
            "missing checksum": lines[:-1],
            "malformed checksum": lines[:-1] + ["# checksum: abc"],
            "trailing output": lines + ["unexpected output"],
            "inconsistent byte timing": lines[:6] + [lines[6].rsplit(",", 1)[0] + ",999.000000"] + lines[7:],
        }
        for label, rows in malformed.items():
            with self.subTest(label=label), self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_formscan("\n".join(rows), "automatic", 100000)
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.parse_formscan(formscan_output("scalar"), "automatic", 100000)

    def test_summary_is_median_and_observed_range(self):
        result = benchmark.bmf({"case": [1000.0, 5.0, 3.0, 4.0, 6.0]})
        self.assertEqual(result, {"case": {"latency": {"value": 5.0, "lower_value": 3.0, "upper_value": 1000.0}}})

    def test_codec_rejects_missing_duplicate_and_unknown_cases(self):
        lines = codec_output().splitlines()
        malformed = [
            lines[:3] + lines[4:],
            lines[:3] + [lines[3]] + lines[3:],
            lines[:3] + [lines[3].replace("literal", "other")] + lines[4:],
            lines[:-1],
            lines + ["unexpected output"],
            [lines[0], lines[1].replace("10000", "1")] + lines[2:],
        ]
        for rows in malformed:
            with self.subTest(rows=rows[:4]), self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_codec("\n".join(rows), "automatic", 10000)
        with self.assertRaises(benchmark.BenchmarkError):
            benchmark.parse_codec(codec_output("scalar"), "automatic", 10000)

    def test_validation_rejects_missing_duplicate_and_unknown_samples(self):
        lines = validation_output().splitlines()
        malformed = [
            lines[:5] + lines[6:],
            lines[:5] + [lines[5]] + lines[5:],
            lines[:5] + [lines[5].replace(",1,", ",6,")] + lines[6:],
            lines[:5] + [lines[5].replace("valid", "other")] + lines[6:],
            lines[:-1],
            lines[:-1] + ["# checksum: abc"],
            lines[:2] + [lines[2].replace("100000", "1")] + lines[3:],
        ]
        for rows in malformed:
            with self.subTest(rows=rows[:6]), self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_validation("\n".join(rows), "automatic", 100000)

    def test_nonpositive_and_nonfinite_timings_never_become_metrics(self):
        for value in ("0", "-0.5", "nan", "inf", "-inf", "bad"):
            with self.subTest(value=value):
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.parse_codec(codec_output(elapsed=value), "automatic", 10000)
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.parse_validation(validation_output().replace(",1.500", f",{value}", 1), "automatic", 100000)
                for column in (5, 6):
                    lines = formscan_output().splitlines()
                    row = lines[6].split(",")
                    row[column] = value
                    lines[6] = ",".join(row)
                    with self.subTest(column=column), self.assertRaises(benchmark.BenchmarkError):
                        benchmark.parse_formscan("\n".join(lines), "automatic", 100000)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        for suffix in ("", "_scalar", "_validate", "_validate_scalar", "_formscan", "_formscan_portable", "_formscan_compiled"):
            (self.bin_dir / ("simdurl_bench" + suffix)).write_bytes(b"fake benchmark for mocked subprocess")
        self.output_dir = self.root / "results"

    def invoke(self, *args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = benchmark.main([
                "--bin-dir", str(self.bin_dir), "--output-dir", str(self.output_dir),
                "--codec-repeats", "3", "--codec-iterations", "10000", "--commit", "test-commit", *args,
            ])
        return status, stdout.getvalue(), stderr.getvalue()

    @staticmethod
    def fake_process(command, **kwargs):
        executable = Path(command[0]).name
        build = ("compiled" if executable.endswith("_compiled") else
                 "scalar" if executable.endswith(("_scalar", "_portable")) else "automatic")
        fixture = (formscan_output if "formscan" in executable
                   else validation_output if "validate" in executable else codec_output)
        return subprocess.CompletedProcess(command, 0, fixture(build, int(command[1])), "")

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_complete_run_outputs_only_bmf_and_retains_evidence(self, subprocess_run):
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stderr), (0, ""))
        result = json.loads(stdout)
        self.assertEqual(len(result), 516)
        legacy = {name: value for name, value in result.items() if not name.startswith("formscan/")}
        expected_legacy = {}
        for build in ("automatic", "scalar"):
            codec, _ = benchmark.parse_codec(codec_output(build), build, 10000)
            validation, _ = benchmark.parse_validation(validation_output(build), build, 100000)
            expected_legacy.update(benchmark.bmf(codec))
            expected_legacy.update(benchmark.bmf(validation))
        self.assertEqual(legacy, expected_legacy)
        self.assertEqual(result, json.loads((self.output_dir / "results.json").read_text()))
        samples = json.loads((self.output_dir / "samples.json").read_text())
        self.assertEqual(len(samples["codec/encode/URI/literal/16/simdurl/automatic"]), 3)
        self.assertEqual(len(samples["validate/C0_DEL/valid/0/simdurl/scalar"]), 5)
        self.assertEqual(samples["formscan/form/plus_short/64/simdurl/scalar"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertEqual(samples["formscan/form/plus_short/64/simdurl/compiled"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertEqual(sum(len(values) for name, values in samples.items() if name.startswith("formscan/")), 900)
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(metadata["commit"], "test-commit")
        self.assertEqual(metadata["formscan_iterations"], 100000)
        self.assertEqual(metadata["formscan_repeats"], 5)
        self.assertEqual(metadata["checksums"]["formscan"], 345678)
        self.assertEqual(len(metadata["executables"]), 7)
        self.assertEqual(len(metadata["executables"]["simdurl_bench_formscan_portable"]["sha256"]), 64)
        self.assertEqual(len(metadata["executables"]["simdurl_bench_formscan_compiled"]["sha256"]), 64)
        self.assertEqual(subprocess_run.call_count, 11)
        self.assertEqual([run["label"] for run in metadata["runs"]], [
            "codec-automatic-1", "codec-scalar-1", "codec-scalar-2", "codec-automatic-2",
            "codec-automatic-3", "codec-scalar-3", "validate-automatic-1", "validate-scalar-1",
            "formscan-automatic-1", "formscan-scalar-1", "formscan-compiled-1",
        ])
        self.assertEqual(len(list((self.output_dir / "raw").glob("*.stdout"))), 11)
        self.assertEqual(len(list((self.output_dir / "raw").glob("*.stderr"))), 11)

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_formscan_iteration_override_reaches_all_three_processes(self, subprocess_run):
        status, stdout, stderr = self.invoke("--formscan-iterations", "100003")
        self.assertEqual((status, stderr), (0, ""))
        self.assertEqual(len(json.loads(stdout)), 516)
        self.assertEqual([call.args[0][1] for call in subprocess_run.call_args_list[-3:]], ["100003"] * 3)
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        self.assertEqual(metadata["formscan_iterations"], 100003)

    @mock.patch.object(benchmark.subprocess, "run")
    def test_formscan_checksum_mismatch_retains_legacy_samples_without_publishing(self, subprocess_run):
        def mismatched(command, **kwargs):
            process = self.fake_process(command, **kwargs)
            if Path(command[0]).name == "simdurl_bench_formscan_portable":
                process.stdout = process.stdout.replace("# checksum: 345678", "# checksum: 999999")
            return process
        subprocess_run.side_effect = mismatched
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("formscan checksums differ", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())
        samples = json.loads((self.output_dir / "samples.json").read_text())
        self.assertEqual(len([name for name in samples if not name.startswith("formscan/")]), 336)
        self.assertTrue((self.output_dir / "raw/formscan-scalar-1.stdout").exists())
        self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_missing_formscan_executable_retains_prior_evidence_without_publishing(self, subprocess_run):
        (self.bin_dir / "simdurl_bench_formscan").unlink()
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("simdurl_bench_formscan", stderr)
        self.assertEqual(subprocess_run.call_count, 8)
        self.assertFalse((self.output_dir / "results.json").exists())
        self.assertEqual(len(json.loads((self.output_dir / "samples.json").read_text())), 336)

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_missing_compiled_formscan_executable_does_not_publish_partial_results(self, subprocess_run):
        (self.bin_dir / "simdurl_bench_formscan_compiled").unlink()
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("simdurl_bench_formscan_compiled", stderr)
        self.assertEqual(subprocess_run.call_count, 10)
        self.assertFalse((self.output_dir / "results.json").exists())
        self.assertEqual(len(json.loads((self.output_dir / "samples.json").read_text())), 456)

    @mock.patch.object(benchmark.subprocess, "run")
    def test_compiled_formscan_checksum_mismatch_does_not_publish(self, subprocess_run):
        def mismatched(command, **kwargs):
            process = self.fake_process(command, **kwargs)
            if Path(command[0]).name == "simdurl_bench_formscan_compiled":
                process.stdout = process.stdout.replace("# checksum: 345678", "# checksum: 999999")
            return process
        subprocess_run.side_effect = mismatched
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("formscan checksums differ", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())
        self.assertTrue((self.output_dir / "raw/formscan-compiled-1.stdout").exists())

    @mock.patch.object(benchmark.subprocess, "run")
    def test_failed_process_keeps_raw_error_and_emits_no_bmf(self, subprocess_run):
        subprocess_run.return_value = subprocess.CompletedProcess([], 7, "partial output", "failed operation")
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("exited with 7", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())
        self.assertEqual((self.output_dir / "raw/codec-automatic-1.stderr").read_text(), "failed operation")
        self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")

    @mock.patch.object(benchmark.subprocess, "run")
    def test_timeout_keeps_partial_output_and_emits_no_bmf(self, subprocess_run):
        subprocess_run.side_effect = subprocess.TimeoutExpired([], 1, output=b"partial", stderr=b"problem")
        status, stdout, stderr = self.invoke("--timeout", "1")
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("process timeout", stderr)
        self.assertEqual((self.output_dir / "raw/codec-automatic-1.stdout").read_text(), "partial")
        self.assertFalse((self.output_dir / "results.json").exists())

    @mock.patch.object(benchmark.subprocess, "run")
    def test_incomplete_successful_process_is_rejected(self, subprocess_run):
        subprocess_run.return_value = subprocess.CompletedProcess([], 0, codec_output().replace("encode URI literal 16 2.500 1.250\n", ""), "")
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("Incomplete codec output", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())

    @mock.patch.object(benchmark.subprocess, "run")
    def test_different_checksums_are_rejected(self, subprocess_run):
        subprocess_run.side_effect = [
            subprocess.CompletedProcess([], 0, codec_output(), ""),
            subprocess.CompletedProcess([], 0, codec_output("scalar").replace("123456", "999999"), ""),
        ]
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("checksums differ", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())

    @mock.patch.object(benchmark.subprocess, "run")
    def test_existing_results_are_preserved_and_not_reused(self, subprocess_run):
        self.output_dir.mkdir()
        previous = self.output_dir / "results.json"
        previous.write_text("previous results")
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("must be empty", stderr)
        self.assertEqual(previous.read_text(), "previous results")
        subprocess_run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
