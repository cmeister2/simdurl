#!/usr/bin/env python3
"""Focused validation of benchmark import and failure handling (stdlib only)."""

import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import benchmark


def calibrated_output(family):
    lines = ["# simdurl calibrated v1", "benchmark,phase,sample,iterations,elapsed_ns"]
    for name in sorted(benchmark.core_names(family)):
        lines.extend([
            f"{name},warmup,0,100,0",
            f"{name},warmup,0,100000,100000000",
            f"{name},calibration,0,100000,40000000",
            f"{name},calibration,0,250000,100000000",
            f"{name},retry,0,250000,49000000",
        ])
        for sample in range(1, 21):
            lines.append(f"{name},sample,{sample},500000,{100000000 + sample * 1000000}")
    lines.append("# checksum: 123456")
    return "\n".join(lines) + "\n"


class CalibratedParserTests(unittest.TestCase):
    def test_only_accepted_batches_contribute_to_median_and_range(self):
        for family in benchmark.CORE_CASES:
            with self.subTest(family=family):
                samples, checksum, batches = benchmark.parse_calibrated(calibrated_output(family), family)
                self.assertEqual(set(samples), benchmark.core_names(family))
                self.assertEqual(checksum, 123456)
                for name, values in samples.items():
                    self.assertEqual(values, list(range(202, 242, 2)))
                    self.assertEqual(len(batches[name]), 25)
                    self.assertEqual(benchmark.bmf({name: values})[name]["latency"],
                                     {"value": 221, "lower_value": 202, "upper_value": 240})

    def test_rejects_missing_short_reordered_or_invalid_batch_evidence(self):
        _, _, good = benchmark.parse_calibrated(calibrated_output("formscan"), "formscan")
        name = next(iter(good))
        changes = {
            "short warmup": lambda rows: rows[1].update(elapsed_ns=99999999),
            "short calibration": lambda rows: rows[3].update(elapsed_ns=99999999),
            "short sample": lambda rows: rows[5].update(elapsed_ns=49999999),
            "duplicate sample": lambda rows: rows[6].update(sample=1),
            "missing sample": lambda rows: rows.pop(),
            "out of order": lambda rows: rows.insert(6, rows.pop(1)),
            "trailing retry": lambda rows: rows.append(dict(rows[4])),
            "invalid phase": lambda rows: rows[0].update(phase="other"),
            "invalid discarded index": lambda rows: rows[0].update(sample=1),
            "zero iterations": lambda rows: rows[0].update(iterations=0),
            "boolean iterations": lambda rows: rows[0].update(iterations=True),
            "negative duration": lambda rows: rows[0].update(elapsed_ns=-1),
            "fractional duration": lambda rows: rows[0].update(elapsed_ns=0.5),
            "unnecessary retry": lambda rows: rows[4].update(elapsed_ns=50000000),
        }
        for label, change in changes.items():
            with self.subTest(label=label):
                batches = copy.deepcopy(good)
                change(batches[name])
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.validate_calibrated_batches(batches, {name})

    def test_rejects_changed_case_set_headers_csv_and_checksum(self):
        lines = calibrated_output("formscan").splitlines()
        for output in (
            "\n".join(lines[1:]),
            "\n".join(lines[:-1]),
            "\n".join(lines + ["extra"]),
            "\n".join(lines).replace("plus_long", "other"),
            "\n".join(lines).replace("# checksum: 123456", "# checksum: invalid"),
            "\n".join(lines).replace(",sample,1,", ",sample,1.0,"),
            "\n".join(lines).replace(",warmup,0,100,0", ",warmup,0,100,NaN"),
            "\n".join(lines[:2] + ['"' + lines[2]] + lines[3:]),
        ):
            with self.subTest(output=output[:80]), self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_calibrated(output, "formscan")


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


def helpers_output(build="automatic", iterations=100000):
    backend = {
        "automatic": "automatic CPU selection (header-only)",
        "scalar": "portable C header-only; compiler-generated SIMD permitted",
        "compiled": "compiled library; configured library CPU selection applies",
    }[build]
    lines = [
        f"# simdurl: {backend}",
        "# comparators: independent portable C; optimization and vectorization enabled",
        "# comparators assume valid arguments; simdurl includes API argument checks",
        "# both variants use noinline wrappers; compiled simdurl retains a separate API boundary",
        "# fixed hex wrappers expose constant lengths and case to both variants",
        "# inplace inputs are already lowercased before timing; no input-reset cost included",
        "# checksums, output sampling and call overhead are included in both timings",
        f"# {iterations} iterations/sample; 5 alternating samples; CPU time; ns/operation",
        "operation,pattern,bytes,length_kind,variant,sample,ns_per_op",
    ]

    def append_case(operation, pattern, length, kind):
        for sample in (1, 3, 5, 2, 4):
            variants = ("portable_C_comparator", "simdurl")
            for variant in variants[::1 if sample % 2 else -1]:
                timing = sample + (10.5 if variant == "portable_C_comparator" else 0.5)
                lines.append(f"{operation},{pattern},{length},{kind},{variant},{sample},{timing:.3f}")

    for length in (0, 1, 8, 15, 16, 17, 20, 31, 32, 33, 48, 63, 64, 65, 128, 512, 4096):
        for operation in ("ascii_copy", "ascii_inplace_already_lowered", "hex_lower", "hex_upper"):
            patterns = (("binary",) if operation.startswith("hex_") else
                        ("mixed_ascii", "unchanged_ascii", "high_bytes"))
            for pattern in patterns:
                if length or pattern in ("mixed_ascii", "binary"):
                    append_case(operation, pattern, length, "runtime")
    for length in (16, 20, 32, 64):
        for operation in ("hex_lower", "hex_upper"):
            append_case(operation, "binary", length, "fixed")
    lines.append("# checksum: 901234")
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

    def test_helpers_preserve_full_runtime_fixed_and_comparator_matrix(self):
        results = {}
        for build in ("automatic", "scalar", "compiled"):
            samples, observed_checksum = benchmark.parse_helpers(helpers_output(build), build, 100000)
            self.assertEqual(observed_checksum, 901234)
            self.assertEqual(len(samples), 280)
            self.assertEqual(sum(len(values) for values in samples.values()), 1400)
            self.assertEqual(samples[f"helpers/ascii_copy/mixed_ascii/0/runtime/simdurl/{build}"],
                             [1.5, 2.5, 3.5, 4.5, 5.5])
            self.assertEqual(samples[f"helpers/hex_lower/binary/64/fixed/portable_C_comparator/{build}"],
                             [11.5, 12.5, 13.5, 14.5, 15.5])
            self.assertNotIn(f"helpers/ascii_copy/high_bytes/0/runtime/simdurl/{build}", samples)
            self.assertNotIn(f"helpers/ascii_copy/mixed_ascii/16/fixed/simdurl/{build}", samples)
            for length in (63, 64, 65):
                self.assertIn(f"helpers/ascii_inplace_already_lowered/high_bytes/{length}/runtime/simdurl/{build}", samples)
                self.assertIn(f"helpers/hex_upper/binary/{length}/runtime/simdurl/{build}", samples)
            results.update(samples)
        self.assertEqual(len(results), 840)

    def test_helpers_reject_other_builds_and_changed_iteration_metadata(self):
        for expected in ("automatic", "scalar", "compiled"):
            for actual in ("automatic", "scalar", "compiled"):
                if actual != expected:
                    with self.subTest(expected=expected, actual=actual), self.assertRaisesRegex(
                            benchmark.BenchmarkError, "header, build, or iteration"):
                        benchmark.parse_helpers(helpers_output(actual), expected, 100000)
        with self.assertRaisesRegex(benchmark.BenchmarkError, "iteration count"):
            benchmark.parse_helpers(helpers_output(iterations=100003), "automatic", 100000)
        self.assertEqual(len(benchmark.parse_helpers(helpers_output(iterations=100003),
                                                     "automatic", 100003)[0]), 280)

    def test_helpers_reject_changed_matrix_samples_headers_and_checksum(self):
        lines = helpers_output().splitlines()
        malformed = {
            "missing sample": lines[:9] + lines[10:],
            "duplicate sample": lines[:9] + [lines[9]] + lines[9:],
            "unknown operation": lines[:9] + [lines[9].replace("ascii_copy", "unknown")] + lines[10:],
            "unknown pattern": lines[:9] + [lines[9].replace("mixed_ascii", "other")] + lines[10:],
            "unknown length": lines[:9] + [lines[9].replace(",0,", ",2,")] + lines[10:],
            "invalid zero pattern": lines[:9] + [lines[9].replace("mixed_ascii", "high_bytes")] + lines[10:],
            "invalid fixed ASCII": lines[:9] + [lines[9].replace("runtime", "fixed")] + lines[10:],
            "unknown variant": lines[:9] + [lines[9].replace("portable_C_comparator", "other")] + lines[10:],
            "invalid sample": lines[:9] + [lines[9].replace(",1,", ",6,")] + lines[10:],
            "missing field": lines[:9] + [lines[9].rsplit(",", 1)[0]] + lines[10:],
            "extra field": lines[:9] + [lines[9] + ",extra"] + lines[10:],
            "invalid CSV": lines[:9] + ['"' + lines[9]] + lines[10:],
            "changed metadata": lines[:5] + ["# inplace inputs are reset before timing"] + lines[6:],
            "changed repeat count": lines[:7] + [lines[7].replace("5 alternating", "4 alternating")] + lines[8:],
            "changed CSV header": lines[:8] + [lines[8].replace("length_kind", "kind")] + lines[9:],
            "missing checksum": lines[:-1],
            "malformed checksum": lines[:-1] + ["# checksum: abc"],
            "trailing output": lines + ["unexpected output"],
            "missing fixed case": [line for line in lines if not line.startswith("hex_upper,binary,64,fixed,")],
            "missing whole comparator": [line for line in lines if ",portable_C_comparator," not in line],
        }
        for label, rows in malformed.items():
            with self.subTest(label=label), self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_helpers("\n".join(rows), "automatic", 100000)

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
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.parse_helpers(helpers_output().replace(",11.500", f",{value}", 1), "automatic", 100000)
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
        for suffix in ("", "_scalar", "_validate", "_validate_scalar", "_formscan", "_formscan_portable", "_formscan_compiled",
                       "_helpers", "_helpers_portable", "_helpers_compiled"):
            (self.bin_dir / ("simdurl_bench" + suffix)).write_bytes(b"fake benchmark for mocked subprocess")
        self.output_dir = self.root / "results"

    def invoke(self, *args, suite="full"):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = benchmark.main([
                "--bin-dir", str(self.bin_dir), "--output-dir", str(self.output_dir),
                "--sampling", "fixed",
                "--codec-repeats", "3", "--codec-iterations", "10000", "--commit", "test-commit",
                *(["--suite", suite] if suite is not None else []), *args,
            ])
        return status, stdout.getvalue(), stderr.getvalue()

    @staticmethod
    def fake_process(command, **kwargs):
        executable = Path(command[0]).name
        build = ("compiled" if executable.endswith("_compiled") else
                 "scalar" if executable.endswith(("_scalar", "_portable")) else "automatic")
        fixture = (helpers_output if "helpers" in executable else
                   formscan_output if "formscan" in executable else
                   validation_output if "validate" in executable else codec_output)
        output = fixture(build, int(command[1]))
        if "--core" in command:
            family, header_rows, fields = (
                ("helpers", 9, 5) if "helpers" in executable else
                ("formscan", 6, 3) if "formscan" in executable else
                ("validate", 5, 4) if "validate" in executable else ("codec", 3, 4)
            )
            lines = output.splitlines()
            rows = [line for line in lines[header_rows:-1]
                    if tuple((line.split() if family == "codec" else line.split(","))[:fields])
                    in benchmark.CORE_CASES[family]]
            output = "\n".join(lines[:header_rows] + rows + lines[-1:]) + "\n"
            if family == "helpers":
                output = output.replace("5 alternating samples", "5 samples")
        return subprocess.CompletedProcess(command, 0, output, "")

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_fixed_core_runs_only_ten_benchmarks_with_existing_names(self, subprocess_run):
        # Core must work with only the four automatic executables installed.
        for executable in self.bin_dir.iterdir():
            if executable.name.endswith(("_scalar", "_portable", "_compiled")):
                executable.unlink()
        status, stdout, stderr = self.invoke(suite=None)
        self.assertEqual((status, stderr), (0, ""))
        result = json.loads(stdout)
        self.assertEqual(set(result), {
            "codec/encode/URI/mixed/128/simdurl/automatic",
            "codec/decode/form/mixed/128/simdurl/automatic",
            "codec/encode/URI/literal/4096/simdurl/automatic",
            "codec/encode/URI/dense/4096/simdurl/automatic",
            "codec/decode/URI/dense/4096/simdurl/automatic",
            "validate/C0_DEL_SPACE/valid/4096/simdurl/automatic",
            "formscan/form/plus_long/16384/simdurl/automatic",
            "helpers/ascii_copy/mixed_ascii/128/runtime/simdurl/automatic",
            "helpers/hex_lower/binary/32/fixed/simdurl/automatic",
            "helpers/hex_lower/binary/4096/runtime/simdurl/automatic",
        })
        self.assertEqual(result, json.loads((self.output_dir / "results.json").read_text()))
        samples = json.loads((self.output_dir / "samples.json").read_text())
        self.assertEqual(set(samples), set(result))
        for name, values in samples.items():
            self.assertEqual(len(values), 3 if name.startswith("codec/") else 5)
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        self.assertEqual((metadata["suite"], metadata["benchmark_count"]), ("core", 10))
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(len(metadata["executables"]), 4)
        self.assertEqual(subprocess_run.call_count, 6)
        self.assertTrue(all(call.args[0][-1] == "--core" for call in subprocess_run.call_args_list))

    @mock.patch.object(benchmark.subprocess, "run")
    def test_default_calibrates_ten_cases_and_preserves_batch_evidence(self, subprocess_run):
        def process(command, **kwargs):
            executable = Path(command[0]).name
            family = next((name for name in ("validate", "formscan", "helpers")
                           if name in executable), "codec")
            return subprocess.CompletedProcess(command, 0, calibrated_output(family), "")

        subprocess_run.side_effect = process
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = benchmark.main([
                "--bin-dir", str(self.bin_dir), "--output-dir", str(self.output_dir),
                "--commit", "test-commit",
            ])
        self.assertEqual(status, 0)
        result = json.loads(stdout.getvalue())
        self.assertEqual(set(result), benchmark.core_names())
        self.assertEqual(subprocess_run.call_count, 4)
        self.assertTrue(all(call.args[0][-1] == "--calibrated"
                            for call in subprocess_run.call_args_list))
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        samples = json.loads((self.output_dir / "samples.json").read_text())
        self.assertEqual(metadata["sampling_policy"], benchmark.CALIBRATED_POLICY)
        for key in ("codec_repeats", "validation_repeats", "formscan_repeats", "helper_repeats"):
            self.assertEqual(metadata[key], 20)
        self.assertEqual(benchmark.validate_calibrated_batches(metadata["batches"]), samples)
        self.assertEqual(benchmark.bmf(samples), result)
        self.assertEqual(metadata["status"], "complete")
        self.assertEqual(len(metadata["executables"]), 4)

    @mock.patch.object(benchmark.subprocess, "run")
    def test_calibrated_failure_retains_raw_output_without_publishing(self, subprocess_run):
        incomplete = calibrated_output("codec").replace(",sample,20,", ",sample,19,")
        subprocess_run.return_value = subprocess.CompletedProcess([], 0, incomplete, "")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = benchmark.main([
                "--bin-dir", str(self.bin_dir), "--output-dir", str(self.output_dir),
            ])
        self.assertEqual((status, stdout.getvalue()), (1, ""))
        self.assertIn("out-of-order", stderr.getvalue())
        self.assertEqual((self.output_dir / "raw/codec-automatic-1.stdout").read_text(), incomplete)
        self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")
        self.assertFalse((self.output_dir / "results.json").exists())

    def test_calibrated_rejects_unsupported_overrides(self):
        for arguments in (["--codec-repeats", "1"], ["--suite", "full", "--sampling", "calibrated"]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    benchmark.main(arguments)
                self.assertEqual(raised.exception.code, 2)

    @mock.patch.object(benchmark.subprocess, "run")
    def test_core_rejects_missing_samples_and_full_output_without_publishing(self, subprocess_run):
        for family, executable, header_rows in (
                ("codec", "simdurl_bench", 3), ("validate", "simdurl_bench_validate", 5),
                ("formscan", "simdurl_bench_formscan", 6), ("helpers", "simdurl_bench_helpers", 9)):
            for malformed in ("missing", "full"):
                with self.subTest(family=family, malformed=malformed):
                    self.output_dir = self.root / f"{family}-{malformed}"

                    def process(command, **kwargs):
                        if Path(command[0]).name != executable:
                            return self.fake_process(command, **kwargs)
                        if malformed == "full":
                            result = self.fake_process(command[:-1], **kwargs)
                            # Test rejection of extra rows, not just the helper header.
                            result.stdout = result.stdout.replace("5 alternating samples", "5 samples")
                        else:
                            result = self.fake_process(command, **kwargs)
                            lines = result.stdout.splitlines()
                            result.stdout = "\n".join(lines[:header_rows] + lines[header_rows + 1:]) + "\n"
                        return result

                    subprocess_run.side_effect = process
                    status, stdout, stderr = self.invoke(suite="core")
                    self.assertEqual((status, stdout), (1, ""))
                    self.assertIn("Incomplete" if malformed == "missing" else "Unexpected", stderr)
                    self.assertFalse((self.output_dir / "results.json").exists())
                    self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")

    @mock.patch.object(benchmark.subprocess, "run")
    def test_core_repeated_codec_checksum_mismatch_is_rejected(self, subprocess_run):
        first = self.fake_process([str(self.bin_dir / "simdurl_bench"), "10000", "--core"])
        second = subprocess.CompletedProcess(first.args, 0, first.stdout.replace("123456", "999999"), "")
        subprocess_run.side_effect = [first, second]
        status, stdout, stderr = self.invoke(suite="core")
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("checksums differ", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_complete_run_outputs_only_bmf_and_retains_evidence(self, subprocess_run):
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stderr), (0, ""))
        result = json.loads(stdout)
        self.assertEqual(len(result), 1356)
        legacy = {name: value for name, value in result.items() if not name.startswith("helpers/")}
        expected_legacy = {}
        for build in ("automatic", "scalar"):
            codec, _ = benchmark.parse_codec(codec_output(build), build, 10000)
            validation, _ = benchmark.parse_validation(validation_output(build), build, 100000)
            expected_legacy.update(benchmark.bmf(codec))
            expected_legacy.update(benchmark.bmf(validation))
        for build in ("automatic", "scalar", "compiled"):
            formscan, _ = benchmark.parse_formscan(formscan_output(build), build, 100000)
            expected_legacy.update(benchmark.bmf(formscan))
        self.assertEqual(len(expected_legacy), 516)
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
        self.assertEqual(metadata["harness_version"], 5)
        self.assertEqual(metadata["sampling_policy"], {"mode": "fixed"})
        self.assertEqual(metadata["suite"], "full")
        self.assertEqual(metadata["benchmark_count"], 1356)
        self.assertEqual(metadata["commit"], "test-commit")
        self.assertEqual(metadata["formscan_iterations"], 100000)
        self.assertEqual(metadata["formscan_repeats"], 5)
        self.assertEqual(metadata["checksums"]["formscan"], 345678)
        self.assertEqual(metadata["helper_iterations"], 500000)
        self.assertEqual(metadata["helper_repeats"], 5)
        self.assertEqual(metadata["checksums"]["helpers"], 901234)
        self.assertEqual(samples["helpers/ascii_copy/mixed_ascii/64/runtime/simdurl/compiled"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertEqual(samples["helpers/hex_upper/binary/64/fixed/simdurl/automatic"], [1.5, 2.5, 3.5, 4.5, 5.5])
        self.assertEqual(sum(len(values) for name, values in samples.items() if name.startswith("helpers/")), 4200)
        self.assertEqual(len(metadata["executables"]), 10)
        self.assertEqual(len(metadata["executables"]["simdurl_bench_formscan_portable"]["sha256"]), 64)
        self.assertEqual(len(metadata["executables"]["simdurl_bench_formscan_compiled"]["sha256"]), 64)
        self.assertEqual(len(metadata["executables"]["simdurl_bench_helpers_compiled"]["sha256"]), 64)
        self.assertEqual(subprocess_run.call_count, 14)
        self.assertEqual([run["label"] for run in metadata["runs"]], [
            "codec-automatic-1", "codec-scalar-1", "codec-scalar-2", "codec-automatic-2",
            "codec-automatic-3", "codec-scalar-3", "validate-automatic-1", "validate-scalar-1",
            "formscan-automatic-1", "formscan-scalar-1", "formscan-compiled-1",
            "helpers-automatic-1", "helpers-scalar-1", "helpers-compiled-1",
        ])
        self.assertEqual(len(list((self.output_dir / "raw").glob("*.stdout"))), 14)
        self.assertEqual(len(list((self.output_dir / "raw").glob("*.stderr"))), 14)

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_formscan_iteration_override_reaches_all_three_processes(self, subprocess_run):
        status, stdout, stderr = self.invoke("--formscan-iterations", "100003")
        self.assertEqual((status, stderr), (0, ""))
        self.assertEqual(len(json.loads(stdout)), 1356)
        calls = [call for call in subprocess_run.call_args_list if "formscan" in Path(call.args[0][0]).name]
        self.assertEqual([call.args[0][1] for call in calls], ["100003"] * 3)
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        self.assertEqual(metadata["formscan_iterations"], 100003)

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_helper_iteration_override_reaches_all_three_processes(self, subprocess_run):
        status, stdout, stderr = self.invoke("--helper-iterations", "123457")
        self.assertEqual((status, stderr), (0, ""))
        self.assertEqual(len(json.loads(stdout)), 1356)
        calls = [call for call in subprocess_run.call_args_list if "helpers" in Path(call.args[0][0]).name]
        self.assertEqual([Path(call.args[0][0]).name for call in calls], [
            "simdurl_bench_helpers", "simdurl_bench_helpers_portable", "simdurl_bench_helpers_compiled",
        ])
        self.assertEqual([call.args[0][1] for call in calls], ["123457"] * 3)
        metadata = json.loads((self.output_dir / "metadata.json").read_text())
        self.assertEqual(metadata["helper_iterations"], 123457)
        self.assertEqual(metadata["helper_repeats"], 5)

    @mock.patch.object(benchmark.subprocess, "run", side_effect=fake_process)
    def test_missing_helper_executable_fails_instead_of_silently_skipping(self, subprocess_run):
        for position, suffix in enumerate(("", "_portable", "_compiled")):
            with self.subTest(suffix=suffix):
                self.output_dir = self.root / ("missing-helpers" + suffix)
                executable = self.bin_dir / ("simdurl_bench_helpers" + suffix)
                original = executable.read_bytes()
                executable.unlink()
                subprocess_run.reset_mock()
                status, stdout, stderr = self.invoke()
                executable.write_bytes(original)
                self.assertEqual((status, stdout), (1, ""))
                self.assertIn(executable.name, stderr)
                self.assertEqual(subprocess_run.call_count, 11 + position)
                self.assertFalse((self.output_dir / "results.json").exists())
                samples = json.loads((self.output_dir / "samples.json").read_text())
                self.assertEqual(len(samples), 516 + 280 * position)
                metadata = json.loads((self.output_dir / "metadata.json").read_text())
                self.assertEqual(metadata["status"], "failed")

    @mock.patch.object(benchmark.subprocess, "run")
    def test_helper_checksum_mismatch_preserves_evidence_without_publishing(self, subprocess_run):
        for position, (suffix, build) in enumerate((("_portable", "scalar"), ("_compiled", "compiled")), 1):
            with self.subTest(build=build):
                self.output_dir = self.root / ("mismatched-helpers" + suffix)

                def mismatched(command, **kwargs):
                    process = self.fake_process(command, **kwargs)
                    if Path(command[0]).name == "simdurl_bench_helpers" + suffix:
                        process.stdout = process.stdout.replace("# checksum: 901234", "# checksum: 999999")
                    return process

                subprocess_run.side_effect = mismatched
                status, stdout, stderr = self.invoke()
                self.assertEqual((status, stdout), (1, ""))
                self.assertIn("helpers checksums differ", stderr)
                self.assertFalse((self.output_dir / "results.json").exists())
                samples = json.loads((self.output_dir / "samples.json").read_text())
                self.assertEqual(len(samples), 516 + 280 * position)
                self.assertEqual(len([name for name in samples if not name.startswith("helpers/")]), 516)
                raw_output = self.output_dir / f"raw/helpers-{build}-1.stdout"
                self.assertIn("# checksum: 999999", raw_output.read_text())
                self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")

    @mock.patch.object(benchmark.subprocess, "run")
    def test_incomplete_helper_output_retains_all_previous_families_without_publishing(self, subprocess_run):
        def incomplete(command, **kwargs):
            process = self.fake_process(command, **kwargs)
            if Path(command[0]).name == "simdurl_bench_helpers":
                lines = process.stdout.splitlines()
                process.stdout = "\n".join(lines[:9] + lines[10:]) + "\n"
            return process

        subprocess_run.side_effect = incomplete
        status, stdout, stderr = self.invoke()
        self.assertEqual((status, stdout), (1, ""))
        self.assertIn("Incomplete helpers output", stderr)
        self.assertFalse((self.output_dir / "results.json").exists())
        samples = json.loads((self.output_dir / "samples.json").read_text())
        self.assertEqual(len(samples), 516)
        self.assertFalse(any(name.startswith("helpers/") for name in samples))
        self.assertTrue((self.output_dir / "raw/helpers-automatic-1.stdout").exists())
        self.assertEqual(subprocess_run.call_count, 12)
        self.assertEqual(json.loads((self.output_dir / "metadata.json").read_text())["status"], "failed")

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
