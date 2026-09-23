#!/usr/bin/env python3
"""Check archive verification and loading at the workflow CLI boundary."""

import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import benchmark_image as image
from test_benchmark_azure_guest import sample_archive


class ImageTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="simdurl-image-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        data, self.config_id, self.manifest_id, self.index_id = sample_archive(nested_index=True)
        self.archive = self.root / "image.tar.gz"
        self.archive.write_bytes(data)
        self.id_file = self.root / "image-id.txt"
        self.id_file.write_text(self.index_id + "\n")
        self.reference = "simdurl-bencher:guest-test"
        self.evidence = self.root / "evidence"

    def arguments(self, command):
        args = [command, "--archive", str(self.archive), "--image", self.reference]
        if command == "load":
            args += ["--image-id-file", str(self.id_file), "--evidence-dir", str(self.evidence)]
        return args

    def run_cli(self, args, loaded=None, load_error=None, inspect_error=None):
        with mock.patch.object(image.subprocess, "run", side_effect=load_error) as load, \
                mock.patch.object(image.subprocess, "check_output",
                                  return_value=(loaded or self.config_id) + "\n",
                                  side_effect=inspect_error) as inspect, \
                contextlib.redirect_stderr(io.StringIO()):
            result = image.main(args)
        return result, load, inspect

    def test_verify_saved_id_needs_no_docker(self):
        result, load, inspect = self.run_cli(
            self.arguments("verify") + ["--image-id-file", str(self.id_file)])
        self.assertEqual(result, 0)
        load.assert_not_called()
        inspect.assert_not_called()

    def test_verify_local_image_inspects_only_requested_tag(self):
        result, load, inspect = self.run_cli(self.arguments("verify"))
        self.assertEqual(result, 0)
        load.assert_not_called()
        inspect.assert_called_once_with(
            ["docker", "image", "inspect", "--format", "{{.Id}}", self.reference],
            text=True, timeout=60)

    def test_wrong_tag_or_builder_id_stops_before_docker(self):
        for field in ("tag", "id"):
            with self.subTest(field=field):
                args = self.arguments("load")
                if field == "tag":
                    args[args.index("--image") + 1] = "simdurl-bencher:unexpected"
                else:
                    self.id_file.write_text("sha256:" + "f" * 64 + "\n")
                result, load, inspect = self.run_cli(args)
                self.assertEqual(result, 1)
                load.assert_not_called()
                inspect.assert_not_called()
                self.assertFalse(self.evidence.exists())

    def test_load_preserves_proven_identity_across_docker_stores(self):
        for loaded in (self.config_id, self.manifest_id, self.index_id):
            with self.subTest(loaded=loaded):
                result, load, _ = self.run_cli(self.arguments("load"), loaded=loaded)
                self.assertEqual(result, 0)
                load.assert_called_once_with(
                    ["docker", "load", "--input", str(self.archive)], check=True, timeout=300)
                self.assertEqual((self.evidence / "controller-image-id.txt").read_text(), loaded + "\n")
                identity = json.loads((self.evidence / "archive-image.json").read_text())
                self.assertEqual(identity["reference"], self.reference)
                self.assertIn(loaded, identity["image_ids"])

    def test_unrelated_loaded_id_has_no_success_evidence(self):
        result, load, _ = self.run_cli(self.arguments("load"), loaded="sha256:" + "f" * 64)
        self.assertEqual(result, 1)
        load.assert_called_once()
        self.assertFalse(self.evidence.exists())

    def test_docker_failures_have_no_success_evidence(self):
        for errors in (
            {"load_error": subprocess.CalledProcessError(1, ["docker", "load"])},
            {"inspect_error": subprocess.TimeoutExpired(["docker", "image", "inspect"], 60)},
        ):
            with self.subTest(errors=errors):
                result, load, inspect = self.run_cli(self.arguments("load"), **errors)
                self.assertEqual(result, 1)
                load.assert_called_once()
                if "load_error" in errors:
                    inspect.assert_not_called()
                self.assertFalse(self.evidence.exists())

    def test_load_requires_builder_id_and_evidence_destination(self):
        for option in ("--image-id-file", "--evidence-dir"):
            with self.subTest(option=option):
                args = self.arguments("load")
                index = args.index(option)
                del args[index:index + 2]
                with self.assertRaises(SystemExit) as raised:
                    self.run_cli(args)
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
