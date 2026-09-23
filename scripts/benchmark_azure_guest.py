#!/usr/bin/env python3
"""Run one benchmark image on a disposable Azure VM using scoped blob URLs."""

import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
import uuid


MAX_OUTPUT_BYTES = 128 * 1024 * 1024
MAX_ARCHIVE_BYTES = 1024 * 1024 * 1024
MAX_DIAGNOSTIC_BYTES = 16 * 1024
CHUNK_BYTES = 64 * 1024
OUTPUT_NAMES = ("results.json", "evidence.json", "host.json")
URL_VARIABLES = {
    "results.json": "SIMDURL_RESULTS_URL",
    "evidence.json": "SIMDURL_EVIDENCE_URL",
    "host.json": "SIMDURL_HOST_URL",
    "completion.json": "SIMDURL_COMPLETION_URL",
}


class GuestError(Exception):
    """A failure whose details must not accidentally reveal a signed URL."""

    def __init__(self, message, details=None):
        super().__init__(message)
        self.details = details or {}


def diagnostic_text(value):
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    value = value or ""
    # Host command arguments never need blob URLs. Redact whole URLs and any
    # standalone SAS signatures before clipping, so clipping cannot hide their
    # identifying prefix while leaving a credential in the retained tail.
    value = re.sub(r"https?://[^\s\"'<>]+", "[redacted URL]", value, flags=re.IGNORECASE)
    value = re.sub(r"\bsig=[^&\s\"'<>]+", "sig=[redacted]", value, flags=re.IGNORECASE)
    encoded = value.encode("utf-8")
    if len(encoded) > MAX_DIAGNOSTIC_BYTES:
        value = "[truncated]\n" + encoded[-(MAX_DIAGNOSTIC_BYTES - 12):].decode("utf-8", errors="ignore")
    return value


def diagnostic_details(value):
    if isinstance(value, (str, bytes)):
        return diagnostic_text(value)
    if isinstance(value, dict):
        return {diagnostic_text(key): diagnostic_details(item) for key, item in value.items()}
    if isinstance(value, list):
        return [diagnostic_details(item) for item in value]
    return value


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, newurl):
        raise GuestError("Unexpected blob redirect")


def https_request(url, **kwargs):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise GuestError("A blob URL must use HTTPS")
    request = urllib.request.Request(url, **kwargs)
    return urllib.request.build_opener(NoRedirect()).open(request, timeout=60)


def download_archive(url, destination, expected_sha256):
    digest = hashlib.sha256()
    size = 0
    with https_request(url) as response, destination.open("wb") as output:
        while chunk := response.read(CHUNK_BYTES):
            size += len(chunk)
            if size > MAX_ARCHIVE_BYTES:
                raise GuestError("Input archive exceeds the size limit")
            digest.update(chunk)
            output.write(chunk)
    if digest.hexdigest() != expected_sha256:
        raise GuestError("Input archive digest does not match")


def archive_image(path, expected_image_id):
    """Prove one tagged image's config/manifest/index identities without extraction.

    Classic Docker identifies an image by its config hash; containerd can use
    its manifest or index hash instead. Follow the archive's digest-checked OCI
    descriptors so either store can identify the same downloaded image.
    """
    metadata_limit = 2 * 1024 * 1024
    digest_pattern = re.compile(r"sha256:[a-f0-9]{64}\Z")
    manifest_types = {"application/vnd.oci.image.manifest.v1+json",
                      "application/vnd.docker.distribution.manifest.v2+json"}
    index_types = {"application/vnd.oci.image.index.v1+json",
                   "application/vnd.docker.distribution.manifest.list.v2+json"}
    try:
        with tarfile.open(path, "r:*") as archive:
            members = archive.getmembers()
            names = {member.name: member for member in members}
            if len(names) != len(members):
                raise GuestError("Image archive contains duplicate member names")

            def read_metadata(name):
                member = names.get(name)
                if member is None or not member.isfile() or member.size > metadata_limit:
                    raise GuestError("Image archive metadata is missing, linked, or oversized")
                return archive.extractfile(member).read()

            legacy = json.loads(read_metadata("manifest.json"))
            if not isinstance(legacy, list) or len(legacy) != 1 or not isinstance(legacy[0], dict):
                raise GuestError("Image archive must contain exactly one image")
            entry = legacy[0]
            tags = entry.get("RepoTags")
            if not isinstance(tags, list) or len(tags) != 1 or not isinstance(tags[0], str):
                raise GuestError("Image archive must contain exactly one existing tag")
            reference = tags[0]
            if len(reference) > 255 or not re.fullmatch(
                    r"[a-z0-9][a-z0-9._:/-]*:[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", reference):
                raise GuestError("Image archive tag is invalid")
            config_name = entry.get("Config")
            if not isinstance(config_name, str):
                raise GuestError("Image archive lacks a config name")
            match = re.fullmatch(r"(?:blobs/sha256/([a-f0-9]{64})|([a-f0-9]{64})\.json)", config_name)
            if not match:
                raise GuestError("Image archive config name is not a content digest")
            config_bytes = read_metadata(config_name)
            config_digest = "sha256:" + hashlib.sha256(config_bytes).hexdigest()
            if config_digest != "sha256:" + (match.group(1) or match.group(2)):
                raise GuestError("Image archive config digest does not match its content")
            config = json.loads(config_bytes)
            if not isinstance(config, dict) or config.get("os") != "linux" or config.get("architecture") != "amd64":
                raise GuestError("Image archive must describe linux/amd64")
            layers = entry.get("Layers")
            if not isinstance(layers, list) or not all(isinstance(layer, str) for layer in layers):
                raise GuestError("Image archive layer list is invalid")
            identities = {config_digest}

            def follow(descriptor, depth=0):
                if depth > 4 or not isinstance(descriptor, dict):
                    raise GuestError("Invalid image descriptor chain")
                digest = descriptor.get("digest")
                if not isinstance(digest, str) or not digest_pattern.fullmatch(digest):
                    raise GuestError("Image descriptor digest is invalid")
                data = read_metadata("blobs/sha256/" + digest[7:])
                if (type(descriptor.get("size")) is not int or descriptor["size"] != len(data)
                        or "sha256:" + hashlib.sha256(data).hexdigest() != digest):
                    raise GuestError("Image descriptor digest or size does not match its content")
                document = json.loads(data)
                if not isinstance(document, dict) or document.get("schemaVersion") != 2:
                    raise GuestError("Image descriptor document is invalid")
                media_type = descriptor.get("mediaType")
                if document.get("mediaType", media_type) != media_type:
                    raise GuestError("Image descriptor media type does not match")
                if media_type in index_types:
                    children = document.get("manifests")
                    if not isinstance(children, list) or len(children) != 1:
                        raise GuestError("Image index must contain exactly one image")
                    follow(children[0], depth + 1)
                elif media_type in manifest_types:
                    linked_config = document.get("config", {})
                    if (not isinstance(linked_config, dict) or linked_config.get("digest") != config_digest
                            or linked_config.get("size") != len(config_bytes)):
                        raise GuestError("Image descriptor points to a different config")
                    linked_layers = document.get("layers")
                    if (not isinstance(linked_layers, list)
                            or any(not isinstance(layer, dict) for layer in linked_layers)
                            or [layer.get("digest") for layer in linked_layers]
                            != ["sha256:" + layer.removeprefix("blobs/sha256/") for layer in layers]):
                        raise GuestError("Image descriptor points to different layers")
                else:
                    raise GuestError("Unsupported image descriptor media type")
                identities.add(digest)

            if "index.json" in names:
                index_bytes = read_metadata("index.json")
                index = json.loads(index_bytes)
                if not isinstance(index, dict) or index.get("schemaVersion") != 2:
                    raise GuestError("Image archive index is invalid")
                descriptors = index.get("manifests")
                if not isinstance(descriptors, list) or len(descriptors) != 1:
                    raise GuestError("Image archive index must contain exactly one image")
                follow(descriptors[0])
                identities.add("sha256:" + hashlib.sha256(index_bytes).hexdigest())
            if expected_image_id not in identities:
                raise GuestError("Requested source image is not proven by the archive")
            return {"reference": reference, "config_digest": config_digest,
                    "image_ids": sorted(identities)}
    except (tarfile.TarError, OSError, ValueError) as error:
        raise GuestError("Cannot read benchmark image archive", {"error_type": type(error).__name__}) from None


def upload_blob(url, path):
    size = path.stat().st_size
    if size > MAX_OUTPUT_BYTES:
        raise GuestError("Output exceeds the size limit")
    with path.open("rb") as data:
        with https_request(url, data=data, method="PUT", headers={
            "x-ms-blob-type": "BlockBlob", "x-ms-version": "2023-11-03",
            "Content-Length": str(size), "Content-Type": "application/json",
        }) as response:
            if response.status != 201:
                raise GuestError("Blob upload was not accepted")


def boot_id():
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def command_output(command):
    details = {"command": command}
    try:
        process = subprocess.run(command, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=60, check=False)
    except subprocess.TimeoutExpired as error:
        details.update(exit_code=None, timeout=True, stdout=error.stdout, stderr=error.stderr)
        raise GuestError("A host or Docker command timed out", details) from None
    except OSError as error:
        details.update(exit_code=None, error_type=type(error).__name__, errno=error.errno)
        raise GuestError("A host or Docker command could not start", details) from None
    if process.returncode:
        details.update(exit_code=process.returncode, stdout=process.stdout, stderr=process.stderr)
        raise GuestError("A host or Docker command failed", details)
    return process.stdout


def collect_host():
    cpuinfo = Path("/proc/cpuinfo").read_text()
    models, flags = set(), set()
    for line in cpuinfo.splitlines():
        key, separator, value = line.partition(":")
        if separator and key.strip() == "model name":
            models.add(value.strip())
        elif separator and key.strip() == "flags":
            flags.update(value.split())
    allowed = sorted(os.sched_getaffinity(0))
    if not allowed:
        raise GuestError("No available guest CPU")
    docker_info = json.loads(command_output(["docker", "info", "--format", "{{json .}}"]))
    return {
        "schema_version": 1,
        "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "kernel": platform.uname()._asdict(),
        "cpu_models": sorted(models), "cpu_flags": sorted(flags),
        "allowed_cpus": allowed, "selected_cpu": allowed[0],
        "lscpu": json.loads(command_output(["lscpu", "--json"])),
        "docker_version": json.loads(command_output(["docker", "version", "--format", "{{json .}}"])),
        "docker_info": {key: docker_info.get(key) for key in ("Driver", "DockerRootDir", "DriverStatus")},
        "os_release": Path("/etc/os-release").read_text(),
    }


def bounded_process(command, stdout_path, stderr_path, timeout):
    """Stream both pipes, enforcing the output cap without buffering them in RAM."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + timeout
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr, \
                selectors.DefaultSelector() as selector:
            for stream, output in ((process.stdout, stdout), (process.stderr, stderr)):
                selector.register(stream, selectors.EVENT_READ, (output, 0))
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("Benchmark exceeded its timeout")
                for key, _ in selector.select(min(remaining, 1)):
                    chunk = os.read(key.fileobj.fileno(), CHUNK_BYTES)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    output, size = key.data
                    output.write(chunk[:MAX_OUTPUT_BYTES - size])
                    size += len(chunk)
                    if size > MAX_OUTPUT_BYTES:
                        raise GuestError("Benchmark output exceeds the size limit")
                    selector.modify(key.fileobj, selectors.EVENT_READ, (output, size))
            return process.wait(timeout=max(0.001, deadline - time.monotonic()))
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
        process.stderr.close()


def benchmark_command(image_id, cpu, name, smoke):
    command = [
        "docker", "run", "--rm", "--pull=never", "--name", name, "--network", "none",
        "--cpuset-cpus", str(cpu), "--cap-drop", "ALL",
        "--security-opt", "no-new-privileges",
        "-e", "SIMDURL_BENCH_REQUIRE_VBMI2=1", image_id, "--suite", "core",
    ]
    if smoke:
        command.extend([
            "--codec-repeats", "1", "--codec-iterations", "10000",
            "--validation-iterations", "10000", "--formscan-iterations", "20000",
            "--helper-iterations", "10000",
        ])
    return command


def run_benchmark(image_id, cpu, smoke, directory, timeout):
    name = "simdurl-benchmark-" + uuid.uuid4().hex
    try:
        return bounded_process(
            benchmark_command(image_id, cpu, name, smoke), directory / "results.json",
            directory / "evidence.json", timeout,
        )
    finally:
        # Killing the Docker client does not reliably stop its container.
        # A normal --rm completion makes this an inexpensive failed lookup.
        try:
            subprocess.run(["docker", "rm", "--force", name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=30, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass


def write_json(path, value):
    data = (json.dumps(value, sort_keys=True) + "\n").encode()
    if len(data) > MAX_OUTPUT_BYTES:
        raise GuestError("JSON output exceeds the size limit")
    path.write_bytes(data)


def record_failure(path, stage, error):
    # Network exception strings can contain signed URLs. Only our own errors
    # carry messages and host-command diagnostics, with explicit redaction.
    details = {"stage": stage, "type": type(error).__name__}
    if isinstance(error, GuestError):
        details.update(message=diagnostic_text(str(error)), details=diagnostic_details(error.details))
    data = ("\n" + json.dumps({"simdurl_guest_error": details}) + "\n").encode()
    with path.open("r+b") as output:
        size = output.seek(0, os.SEEK_END)
        if size + len(data) > MAX_OUTPUT_BYTES:
            output.truncate(max(0, MAX_OUTPUT_BYTES - len(data)))
            output.seek(0, os.SEEK_END)
        output.write(data)


def file_record(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(CHUNK_BYTES):
            digest.update(chunk)
    return {"sha256": digest.hexdigest(), "size": path.stat().st_size}


def run_guest(environment, directory):
    for name in OUTPUT_NAMES:
        (directory / name).touch()
    host = {"schema_version": 1}
    before = after = None
    exit_code = 1
    stage = "configuration"
    commit = environment.get("SIMDURL_COMMIT", "")
    image_id = environment.get("SIMDURL_IMAGE_ID", "")
    try:
        expected_sha256 = environment.get("SIMDURL_INPUT_SHA256", "")
        if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256):
            raise GuestError("Invalid archive digest")
        if not re.fullmatch(r"sha256:[a-f0-9]{64}", image_id):
            raise GuestError("Invalid image ID")
        if not re.fullmatch(r"[a-f0-9]{40}", commit):
            raise GuestError("Invalid commit")
        timeout = int(environment.get("SIMDURL_TIMEOUT", "900"))
        if not 1 <= timeout <= 3600 or environment.get("SIMDURL_SMOKE", "0") not in ("0", "1"):
            raise GuestError("Invalid benchmark limits")
        stage = "host_metadata"
        before = boot_id()
        host = collect_host()
        stage = "download"
        archive = directory / "image.tar.gz"
        download_archive(environment["SIMDURL_INPUT_URL"], archive, expected_sha256)
        stage = "image_archive"
        image = archive_image(archive, image_id)
        host["archive_image"] = image
        stage = "image_load"
        host["image_load_stdout"] = diagnostic_text(command_output(["docker", "load", "--input", str(archive)]))
        stage = "image_inspect"
        observed_image_id = command_output(["docker", "image", "inspect", "--format", "{{.Id}}", image["reference"]]).strip()
        host["loaded_image_id"] = observed_image_id
        if observed_image_id not in image["image_ids"]:
            raise GuestError("Loaded image ID is not proven by the archive", {
                "expected_image_ids": image["image_ids"], "observed_image_id": observed_image_id,
            })
        archive.unlink()
        stage = "benchmark"
        exit_code = run_benchmark(image["reference"], host["selected_cpu"],
                                  environment.get("SIMDURL_SMOKE", "0") == "1", directory, timeout)
    except Exception as error:
        record_failure(directory / "evidence.json", stage, error)
        exit_code = 1
    finally:
        try:
            after = boot_id()
            if before != after:
                exit_code = 1
        except OSError as error:
            record_failure(directory / "evidence.json", "boot_id", error)
            exit_code = 1
        host.update(boot_id_before=before, boot_id_after=after)
        write_json(directory / "host.json", host)

    records = {name: file_record(directory / name) for name in OUTPUT_NAMES}
    for name in OUTPUT_NAMES:
        try:
            upload_blob(environment[URL_VARIABLES[name]], directory / name)
        except Exception as error:
            # Continue so other evidence survives a failed individual upload.
            print(f"Guest upload failed for {name} ({type(error).__name__})", file=sys.stderr)
            exit_code = 1
    completion = {
        "schema_version": 1, "commit": commit, "image_id": image_id,
        "exit_code": exit_code, "boot_id_before": before, "boot_id_after": after,
        "files": records,
    }
    write_json(directory / "completion.json", completion)
    try:
        upload_blob(environment["SIMDURL_COMPLETION_URL"], directory / "completion.json")
    except Exception as error:
        print(f"Guest completion upload failed ({type(error).__name__})", file=sys.stderr)
        return 1
    print(f"Benchmark guest completed with exit code {exit_code}")
    return exit_code


def main():
    try:
        with tempfile.TemporaryDirectory(prefix="simdurl-azure-") as directory:
            return run_guest(os.environ, Path(directory))
    except Exception as error:
        # Even exceptional local I/O failures must not print a chained HTTP
        # exception traceback containing one of the protected signed URLs.
        print(f"Guest failed outside the benchmark ({type(error).__name__})", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
