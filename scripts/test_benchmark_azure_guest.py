#!/usr/bin/env python3
"""Exercise the disposable guest without Docker, Azure, or signed credentials."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

import benchmark_azure_guest as guest


def tar_bytes(entries):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for name, data in entries.items():
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    return output.getvalue()


def sample_archive(tags=None, nested_index=False):
    config = json.dumps({"os": "linux", "architecture": "amd64",
                         "rootfs": {"type": "layers", "diff_ids": []}}).encode()
    config_id = "sha256:" + hashlib.sha256(config).hexdigest()
    manifest_type = "application/vnd.oci.image.manifest.v1+json"
    manifest = json.dumps({"schemaVersion": 2, "mediaType": manifest_type,
                           "config": {"digest": config_id, "size": len(config)}, "layers": []}).encode()
    manifest_id = "sha256:" + hashlib.sha256(manifest).hexdigest()
    descriptor = {"mediaType": manifest_type, "digest": manifest_id, "size": len(manifest)}
    entries = {"blobs/sha256/" + config_id[7:]: config,
               "blobs/sha256/" + manifest_id[7:]: manifest}
    index_id = None
    if nested_index:
        index_type = "application/vnd.oci.image.index.v1+json"
        index = json.dumps({"schemaVersion": 2, "mediaType": index_type, "manifests": [descriptor]}).encode()
        index_id = "sha256:" + hashlib.sha256(index).hexdigest()
        entries["blobs/sha256/" + index_id[7:]] = index
        descriptor = {"mediaType": index_type, "digest": index_id, "size": len(index)}
    entries["index.json"] = json.dumps({"schemaVersion": 2, "manifests": [descriptor]}).encode()
    entries["manifest.json"] = json.dumps([{
        "Config": "blobs/sha256/" + config_id[7:], "Layers": [],
        "RepoTags": ["simdurl-bencher:guest-test"] if tags is None else tags,
    }]).encode()
    return tar_bytes(entries), config_id, manifest_id, index_id


class GuestTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="simdurl-guest-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.archive, self.image_id, self.manifest_id, _ = sample_archive()
        self.image_ref = "simdurl-bencher:guest-test"
        self.environment = {
            "SIMDURL_COMMIT": "a" * 40, "SIMDURL_IMAGE_ID": self.image_id,
            "SIMDURL_INPUT_SHA256": hashlib.sha256(self.archive).hexdigest(),
            "SIMDURL_INPUT_URL": "https://unit.test/image?sig=secret",
            "SIMDURL_TIMEOUT": "900", "SIMDURL_SMOKE": "0",
        }
        self.environment.update({variable: f"https://unit.test/{name}?sig=secret"
                                 for name, variable in guest.URL_VARIABLES.items()})
        self.uploads = []
        self.output = io.StringIO()
        self.error = io.StringIO()

    def benchmark(self, image_ref, cpu, smoke, directory, timeout):
        self.assertEqual(image_ref, self.image_ref)
        self.assertEqual(cpu, 2)
        self.assertFalse(smoke)
        self.assertEqual(timeout, 900)
        (directory / "results.json").write_text('{"metric":{"latency":{"value":1}}}\n')
        (directory / "evidence.json").write_text('{"simdurl_evidence":{"preflight":{"status":"passed"}}}\n')
        return 0

    def download(self, url, path, digest):
        self.assertEqual(url, self.environment["SIMDURL_INPUT_URL"])
        self.assertEqual(digest, self.environment["SIMDURL_INPUT_SHA256"])
        path.write_bytes(self.archive)

    def upload(self, url, path):
        self.assertEqual(url, self.environment[guest.URL_VARIABLES[path.name]])
        self.uploads.append((path.name, path.read_bytes()))

    def run_guest(self, benchmark=None, download=None, image_id=None, upload=None, boots=None,
                  command_results=None):
        with mock.patch.object(guest, "collect_host", return_value={"selected_cpu": 2}), \
                mock.patch.object(guest, "boot_id", side_effect=boots or ["boot-1", "boot-1"]), \
                mock.patch.object(guest, "download_archive", side_effect=download or self.download) as fetch, \
                mock.patch.object(guest, "command_output", side_effect=command_results or ["loaded", image_id or self.image_id]) as command, \
                mock.patch.object(guest, "run_benchmark", side_effect=benchmark or self.benchmark) as run, \
                mock.patch.object(guest, "upload_blob", side_effect=upload or self.upload), \
                contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.error):
            status = guest.run_guest(self.environment, self.root)
        self.assertNotIn("secret", self.output.getvalue() + self.error.getvalue())
        self.assertNotIn("https://", self.output.getvalue() + self.error.getvalue())
        return status, fetch, command, run

    def completion(self):
        self.assertEqual(self.uploads[-1][0], "completion.json")
        return json.loads(self.uploads[-1][1])

    def test_one_image_uploads_three_artifacts_then_completion_with_hashes(self):
        status, _, commands, run = self.run_guest()
        self.assertEqual(status, 0)
        self.assertEqual([name for name, _ in self.uploads], [*guest.OUTPUT_NAMES, "completion.json"])
        completion = self.completion()
        self.assertEqual(completion["schema_version"], 1)
        self.assertEqual(completion["commit"], self.environment["SIMDURL_COMMIT"])
        self.assertEqual(completion["image_id"], self.image_id)
        self.assertEqual(completion["boot_id_before"], completion["boot_id_after"])
        for name, data in self.uploads[:-1]:
            self.assertEqual(completion["files"][name], {
                "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            })
        self.assertEqual(commands.call_args_list[-1].args[0][-1], self.image_ref)
        run.assert_called_once()
        self.assertFalse((self.root / "image.tar.gz").exists())

    def test_failed_native_gate_preserves_failure_and_evidence(self):
        def failed(*args):
            self.benchmark(*args)
            (self.root / "results.json").write_text("")
            (self.root / "evidence.json").write_text("native backend failed\n")
            return 9
        status, *_ = self.run_guest(benchmark=failed)
        self.assertEqual(status, 9)
        self.assertEqual(self.completion()["exit_code"], 9)
        self.assertEqual(dict(self.uploads)["results.json"], b"")
        self.assertIn(b"native backend failed", dict(self.uploads)["evidence.json"])

    def test_download_failure_uploads_partial_evidence_without_docker(self):
        status, _, command, run = self.run_guest(
            download=OSError("https://unit.test/image?sig=secret"),
        )
        self.assertEqual(status, 1)
        command.assert_not_called()
        run.assert_not_called()
        self.assertEqual(self.completion()["exit_code"], 1)
        evidence = dict(self.uploads)["evidence.json"]
        self.assertIn(b'"stage": "download"', evidence)
        self.assertNotIn(b"secret", evidence)

    def test_loaded_image_mismatch_stops_before_benchmark(self):
        status, _, _, run = self.run_guest(image_id="sha256:" + "c" * 64)
        self.assertEqual(status, 1)
        run.assert_not_called()
        self.assertEqual(self.completion()["exit_code"], 1)
        evidence = json.loads(dict(self.uploads)["evidence.json"])["simdurl_guest_error"]
        self.assertEqual(evidence["stage"], "image_inspect")
        self.assertIn(self.image_id, evidence["details"]["expected_image_ids"])
        self.assertEqual(evidence["details"]["observed_image_id"], "sha256:" + "c" * 64)

    def test_docker_load_failure_retains_bounded_redacted_diagnostics(self):
        process = subprocess.CompletedProcess([], 42, "x" * 40000 + "Loaded a partial layer\n",
                                              "unsupported manifest https://unit.test/blob?sig=secret\n"
                                              "standalone sig=secret&sv=2023-11-03\n")
        real_command_output = guest.command_output
        with mock.patch.object(guest.subprocess, "run", return_value=process):
            status, _, _, run = self.run_guest(command_results=real_command_output)
        self.assertEqual(status, 1)
        run.assert_not_called()
        raw = dict(self.uploads)["evidence.json"]
        self.assertNotIn(b"secret", raw)
        self.assertNotIn(b"https://", raw)
        evidence = json.loads(raw)["simdurl_guest_error"]
        self.assertEqual(evidence["stage"], "image_load")
        details = evidence["details"]
        self.assertEqual(details["command"][:2], ["docker", "load"])
        self.assertEqual(details["exit_code"], 42)
        self.assertIn("Loaded a partial layer", details["stdout"])
        self.assertIn("unsupported manifest", details["stderr"])
        self.assertLessEqual(len(details["stdout"].encode()), guest.MAX_DIAGNOSTIC_BYTES)

    def test_host_command_timeout_retains_partial_diagnostics(self):
        timeout = subprocess.TimeoutExpired([], 60, output=b"partial output", stderr=b"timeout sig=secret")
        with mock.patch.object(guest.subprocess, "run", side_effect=timeout):
            with self.assertRaises(guest.GuestError) as raised:
                guest.command_output(["docker", "load", "--input", "image.tar.gz"])
        path = self.root / "failure.json"
        path.touch()
        guest.record_failure(path, "image_load", raised.exception)
        details = json.loads(path.read_text())["simdurl_guest_error"]["details"]
        self.assertTrue(details["timeout"])
        self.assertIsNone(details["exit_code"])
        self.assertEqual(details["stdout"], "partial output")
        self.assertNotIn("secret", details["stderr"])

    def test_host_metadata_keeps_only_allowlisted_docker_storage_fields(self):
        def command(arguments):
            if arguments[:2] == ["docker", "info"]:
                return json.dumps({"Driver": "overlayfs", "DockerRootDir": "/var/lib/docker",
                                   "DriverStatus": [["driver-type", "io.containerd.snapshotter.v1"]],
                                   "HTTPProxy": "https://user:secret@unit.test"})
            return "{}"
        with mock.patch.object(guest, "command_output", side_effect=command):
            host = guest.collect_host()
        self.assertEqual(set(host["docker_info"]), {"Driver", "DockerRootDir", "DriverStatus"})
        self.assertEqual(host["docker_info"]["Driver"], "overlayfs")
        self.assertNotIn("secret", json.dumps(host))

    def test_classic_to_containerd_preserves_source_id_and_runs_existing_tag(self):
        status, _, commands, _ = self.run_guest(image_id=self.manifest_id)
        self.assertEqual(status, 0)
        self.assertEqual(self.completion()["image_id"], self.image_id)
        host = json.loads(dict(self.uploads)["host.json"])
        self.assertEqual(host["loaded_image_id"], self.manifest_id)
        self.assertEqual(host["archive_image"]["reference"], self.image_ref)
        self.assertEqual([call.args[0][1] for call in commands.call_args_list], ["load", "image"])

    def test_containerd_to_classic_accepts_the_same_proven_config(self):
        self.environment["SIMDURL_IMAGE_ID"] = self.manifest_id
        status, *_ = self.run_guest(image_id=self.image_id)
        self.assertEqual(status, 0)
        self.assertEqual(self.completion()["image_id"], self.manifest_id)

    def test_nested_index_digest_is_linked_to_same_config(self):
        archive, config_id, manifest_id, index_id = sample_archive(nested_index=True)
        path = self.root / "nested.tar.gz"
        path.write_bytes(archive)
        image = guest.archive_image(path, index_id)
        self.assertTrue({config_id, manifest_id, index_id}.issubset(image["image_ids"]))

    def test_archive_requires_one_existing_tag(self):
        for tags in ([], [self.image_ref, "another:tag"], ["--privileged"], ["https://unit.test/?sig=secret"]):
            with self.subTest(tags=tags):
                archive, image_id, _, _ = sample_archive(tags=tags)
                path = self.root / "tags.tar.gz"
                path.write_bytes(archive)
                with self.assertRaises(guest.GuestError):
                    guest.archive_image(path, image_id)

    def test_unrelated_source_digest_is_rejected(self):
        path = self.root / "source.tar.gz"
        path.write_bytes(self.archive)
        with self.assertRaises(guest.GuestError):
            guest.archive_image(path, "sha256:" + "f" * 64)

    def test_archive_rejects_wrong_config_or_descriptor_digest_and_multiple_images(self):
        with tarfile.open(fileobj=io.BytesIO(self.archive), mode="r:gz") as archive:
            original = {member.name: archive.extractfile(member).read() for member in archive}
        for kind in ("config", "descriptor", "multiple"):
            with self.subTest(kind=kind):
                entries = dict(original)
                if kind == "config":
                    entries["blobs/sha256/" + self.image_id[7:]] += b" "
                elif kind == "descriptor":
                    entries["blobs/sha256/" + self.manifest_id[7:]] += b" "
                else:
                    legacy = json.loads(entries["manifest.json"])
                    entries["manifest.json"] = json.dumps(legacy * 2).encode()
                path = self.root / "invalid.tar.gz"
                path.write_bytes(tar_bytes(entries))
                with self.assertRaises(guest.GuestError):
                    guest.archive_image(path, self.image_id)

    def test_malformed_tar_fails_before_docker_and_retains_evidence(self):
        def invalid_download(url, path, digest):
            path.write_bytes(b"not a tar archive")
        status, _, command, run = self.run_guest(download=invalid_download)
        self.assertEqual(status, 1)
        command.assert_not_called()
        run.assert_not_called()
        evidence = json.loads(dict(self.uploads)["evidence.json"])["simdurl_guest_error"]
        self.assertEqual(evidence["stage"], "image_archive")

    def test_timeout_preserves_partial_benchmark_output(self):
        def timeout(*args):
            (self.root / "evidence.json").write_text("partial native output\n")
            raise TimeoutError("timeout")
        status, *_ = self.run_guest(benchmark=timeout)
        self.assertEqual(status, 1)
        evidence = dict(self.uploads)["evidence.json"]
        self.assertIn(b"partial native output", evidence)
        self.assertIn(b"TimeoutError", evidence)
        self.assertEqual(self.completion()["exit_code"], 1)

    def test_one_upload_failure_does_not_prevent_remaining_evidence(self):
        def upload(url, path):
            if path.name == "results.json":
                raise OSError("https://unit.test/results.json?sig=secret")
            self.upload(url, path)
        status, *_ = self.run_guest(upload=upload)
        self.assertEqual(status, 1)
        self.assertEqual([name for name, _ in self.uploads], ["evidence.json", "host.json", "completion.json"])
        self.assertEqual(self.completion()["exit_code"], 1)

    def test_completion_upload_failure_is_nonzero_without_url_leak(self):
        def upload(url, path):
            if path.name == "completion.json":
                raise OSError("https://unit.test/completion.json?sig=secret")
            self.upload(url, path)
        status, *_ = self.run_guest(upload=upload)
        self.assertEqual(status, 1)
        self.assertEqual([name for name, _ in self.uploads], list(guest.OUTPUT_NAMES))

    def test_boot_change_invalidates_success(self):
        status, *_ = self.run_guest(boots=["boot-1", "boot-2"])
        self.assertEqual(status, 1)
        self.assertEqual(self.completion()["boot_id_after"], "boot-2")
        self.assertEqual(self.completion()["exit_code"], 1)

    def test_invalid_input_digest_fails_without_download(self):
        self.environment["SIMDURL_INPUT_SHA256"] = "not-a-digest"
        status, fetch, command, run = self.run_guest(boots=["boot-1"])
        self.assertEqual(status, 1)
        fetch.assert_not_called()
        command.assert_not_called()
        run.assert_not_called()

    def test_download_stream_is_digest_checked(self):
        with mock.patch.object(guest, "https_request", return_value=io.BytesIO(self.archive)):
            guest.download_archive("https://unit.test/image", self.root / "image", hashlib.sha256(self.archive).hexdigest())
        self.assertEqual((self.root / "image").read_bytes(), self.archive)
        with mock.patch.object(guest, "https_request", return_value=io.BytesIO(self.archive)):
            with self.assertRaises(guest.GuestError):
                guest.download_archive("https://unit.test/image", self.root / "image", "0" * 64)

    def test_https_is_required_before_network_access(self):
        with mock.patch.object(guest.urllib.request, "build_opener") as opener:
            for url in ("http://unit.test/blob", "https://user:password@unit.test/blob", "file:///tmp/blob"):
                with self.subTest(url=url), self.assertRaises(guest.GuestError):
                    guest.https_request(url)
            opener.assert_not_called()

    def test_blob_upload_uses_exact_bytes_and_block_blob_headers(self):
        path = self.root / "results.json"
        path.write_bytes(b'{"metric":1}\n')
        response = mock.MagicMock()
        response.__enter__.return_value.status = 201
        observed = {}
        def request(url, **kwargs):
            observed.update(url=url, **kwargs)
            observed["payload"] = kwargs["data"].read()
            return response
        with mock.patch.object(guest, "https_request", side_effect=request):
            guest.upload_blob("https://unit.test/output", path)
        self.assertEqual(observed["method"], "PUT")
        self.assertEqual(observed["headers"]["x-ms-blob-type"], "BlockBlob")
        self.assertEqual(int(observed["headers"]["Content-Length"]), len(observed["payload"]))
        self.assertEqual(observed["payload"], path.read_bytes())

    def test_container_command_requires_native_vbmi2_and_one_cpu(self):
        command = guest.benchmark_command(self.image_id, 2, "test-container", False)
        self.assertEqual(command[command.index("--cpuset-cpus") + 1], "2")
        self.assertEqual(command[command.index("--network") + 1], "none")
        self.assertEqual(command[command.index("-e") + 1], "SIMDURL_BENCH_REQUIRE_VBMI2=1")
        self.assertEqual(command[-3:], [self.image_id, "--suite", "core"])
        self.assertIn("no-new-privileges", command)
        self.assertIn("--pull=never", command)
        smoke = guest.benchmark_command(self.image_id, 2, "test-container", True)
        self.assertEqual(smoke[smoke.index("--suite") + 1], "core")
        self.assertIn("--codec-iterations", smoke)

    def test_streamed_output_limit_stops_process_and_keeps_bounded_partial_logs(self):
        stdout, stderr = self.root / "stdout", self.root / "stderr"
        command = [sys.executable, "-c", "import sys; sys.stdout.write('x' * 4096)"]
        with mock.patch.object(guest, "MAX_OUTPUT_BYTES", 128), self.assertRaises(guest.GuestError):
            guest.bounded_process(command, stdout, stderr, 5)
        self.assertEqual(stdout.stat().st_size, 128)
        self.assertLessEqual(stderr.stat().st_size, 128)

    def test_silent_process_timeout_is_enforced(self):
        command = [sys.executable, "-c", "import time; time.sleep(10)"]
        with self.assertRaises((TimeoutError, subprocess.TimeoutExpired)):
            guest.bounded_process(command, self.root / "stdout", self.root / "stderr", 0.05)


if __name__ == "__main__":
    unittest.main()
