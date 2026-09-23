#!/usr/bin/env python3
"""Run one prebuilt benchmark image on a disposable Azure VM (stdlib + az)."""

import argparse
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid

import benchmark

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "infra/azure"))
from lifecycle import expected_resource_ids, parse_time, resource_is_owned, validate_manifest

MAX_FILE_BYTES = 128 * 1024 * 1024
SHA = re.compile(r"[0-9a-f]{40}\Z")
IMAGE_ID = re.compile(r"sha256:[0-9a-f]{64}\Z")
FILES = ("results.json", "evidence.json", "host.json", "completion.json")
RESOURCE_API_VERSIONS = {
    "vm": "2024-07-01", "disk": "2024-03-02",
    "nic": "2024-05-01", "public_ip": "2024-05-01",
}


class AzureError(RuntimeError):
    pass


def now():
    return datetime.now(timezone.utc)


def timestamp(value):
    return value.isoformat().replace("+00:00", "Z")


def redact(message):
    return re.sub(r"https://[^\s\"']+\?[^\s\"']+", "[protected URL]", message)


def command(arguments, *, timeout=180, stdout=None):
    try:
        result = subprocess.run(arguments, stdout=stdout or subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise AzureError(f"{arguments[0]} operation timed out after {timeout}s") from exc
    if result.returncode:
        raise AzureError(redact(result.stderr.decode("utf-8", "replace")[-4000:]))
    return result.stdout or b""


def disposable_public_key():
    """Satisfy VM provisioning without retaining an SSH login credential."""
    with tempfile.TemporaryDirectory(prefix="simdurl-ssh-") as directory:
        key = Path(directory) / "id_ed25519"
        command(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "", "-f", str(key)],
                timeout=30)
        public_key = key.with_suffix(".pub").read_text().strip()
    # Run Command uses the Azure VM agent; neither SSH key file is needed again.
    return public_key


def finite_budget_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def config_from(value, *, validate_budget=True):
    data = json.loads(value if value.lstrip().startswith("{") else Path(value).read_text())
    for field in ("subscription_id", "resource_group", "location", "storage_account",
                  "image_version"):
        if not isinstance(data.get(field), str) or not data[field]:
            raise ValueError(f"configuration requires {field}")
    expected_resource_ids("simdurl-config", data["subscription_id"], data["resource_group"])
    if not re.fullmatch(r"[a-z0-9]{3,24}", data["storage_account"]):
        raise ValueError("invalid storage account name")
    if not re.fullmatch(r"[a-z0-9]+", data["location"]):
        raise ValueError("invalid Azure location")
    if not re.fullmatch(r"\d+\.\d+\.\d+", data["image_version"]):
        raise ValueError("image_version must be a concrete three-part marketplace version")
    # Accept old configurations without carrying a personal key into a new run.
    data.pop("admin_public_key", None)
    lifetime = data.setdefault("lifetime_minutes", 60)
    if isinstance(lifetime, bool) or not isinstance(lifetime, int) or not 60 <= lifetime <= 120:
        raise ValueError("lifetime_minutes must be an integer between 60 and 120")
    if validate_budget and "budget" in data:
        control_group = data.get("control_resource_group")
        if not isinstance(control_group, str) or not re.fullmatch(r"[A-Za-z0-9_.()-]{1,90}", control_group):
            raise ValueError("budget requires a valid control_resource_group")
        budget = data["budget"]
        if not isinstance(budget, dict):
            raise ValueError("budget must be an object")
        if not isinstance(budget.get("name"), str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", budget["name"]):
            raise ValueError("invalid budget name")
        if not isinstance(budget.get("currency"), str) or not re.fullmatch(r"[A-Z]{3}", budget["currency"]):
            raise ValueError("budget currency must be a three-letter uppercase code")
        for field in ("amount", "stop_at"):
            value = budget.get(field)
            if not finite_budget_number(value) or value <= 0:
                raise ValueError(f"budget {field} must be a positive finite number")
        if budget["stop_at"] >= budget["amount"]:
            raise ValueError("budget stop_at must leave headroom below its amount")
        groups = budget.get("resource_groups")
        expected_groups = {data["resource_group"].lower(), control_group.lower()}
        if (not isinstance(groups, list) or len(groups) != 2
                or any(not isinstance(group, str) or not re.fullmatch(r"[A-Za-z0-9_.()-]{1,90}", group) for group in groups)
                or len(expected_groups) != 2
                or {group.lower() for group in groups} != expected_groups):
            raise ValueError("budget resource_groups must contain the distinct compute and control groups")
    return data


class Azure:
    def __init__(self, config):
        self.config = config

    def call(self, *arguments, timeout=180):
        output = command(["az", *map(str, arguments), "--subscription",
                          self.config["subscription_id"], "--only-show-errors", "--output", "json"],
                         timeout=timeout)
        return json.loads(output) if output.strip() else None

    def optional(self, *arguments):
        try:
            return self.call(*arguments)
        except AzureError as exc:
            if any(code in str(exc) for code in ("ResourceNotFound", "BlobNotFound", "DeploymentNotFound",
                                                 "ResourceGroupNotFound", "(NotFound)")):
                return None
            raise

    def storage(self, *arguments, **kwargs):
        return self.call("storage", "blob", *arguments, "--account-name",
                         self.config["storage_account"], "--auth-mode", "login", **kwargs)

    def download(self, container, name, path):
        self.storage("download", "--container-name", container, "--name", name,
                     "--file", str(path), "--overwrite", "true", "--no-progress")
        if path.stat().st_size > MAX_FILE_BYTES:
            raise AzureError(f"oversized evidence file: {name}")

    def read_json(self, container, name, *, optional=False):
        with tempfile.TemporaryDirectory(prefix="simdurl-azure-json-") as tmp:
            path = Path(tmp) / "data.json"
            try:
                self.download(container, name, path)
            except AzureError as exc:
                if optional and "BlobNotFound" in str(exc):
                    return None
                raise
            return json.loads(path.read_text())

    def write_json(self, container, name, data, *, overwrite=True):
        with tempfile.TemporaryDirectory(prefix="simdurl-azure-json-") as tmp:
            path = Path(tmp) / "data.json"
            path.write_text(json.dumps(data, sort_keys=True))
            metadata = {"status": data["status"]} if container == "runs" and isinstance(data.get("status"), str) else None
            self.upload(container, name, path, overwrite=overwrite, metadata=metadata)

    def upload(self, container, name, path, *, overwrite=True, metadata=None):
        metadata_arguments = ["--metadata", *(f"{key}={value}" for key, value in metadata.items())] if metadata else []
        self.storage("upload", "--container-name", container, "--name", name,
                     "--file", str(path), "--overwrite", str(overwrite).lower(),
                     "--no-progress", *metadata_arguments, timeout=300)

    def sas(self, container, name, permissions, expires):
        # Azure CLI's datetime parser rejects fractional seconds.
        expiry = expires.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        token = self.storage("generate-sas", "--container-name", container, "--name", name,
                             "--permissions", permissions, "--expiry", expiry,
                             "--as-user", "--https-only")
        return f"https://{self.config['storage_account']}.blob.core.windows.net/{container}/{name}?{token}"


class AllocationLease:
    """Serialize controllers with an expiring lease, independent of CI concurrency."""

    def __init__(self, azure):
        self.azure = azure
        self.lease_id = str(uuid.uuid4())
        self.stop = threading.Event()
        self.failed = threading.Event()
        self.renewed_at = None
        self.worker = None

    def _operation(self, operation):
        arguments = ["lease", operation, "--container-name", "state", "--blob-name", "allocation.lock",
                     "--timeout", "10"]
        if operation == "acquire":
            arguments += ["--lease-duration", "60", "--proposed-lease-id", self.lease_id]
        else:
            arguments += ["--lease-id", self.lease_id]
        return self.azure.storage(*arguments, timeout=20)

    def __enter__(self):
        if getattr(self.azure, "lease", None) is not None:
            raise AzureError("This controller already holds an allocation lease")
        exists = self.azure.storage("exists", "--container-name", "state", "--name", "allocation.lock",
                                    "--timeout", "10", timeout=20)
        if not exists.get("exists"):
            with tempfile.TemporaryDirectory(prefix="simdurl-allocation-lock-") as temporary:
                path = Path(temporary) / "lock"
                path.write_text("simdurl allocation lock\n")
                try:
                    self.azure.storage("upload", "--container-name", "state", "--name", "allocation.lock",
                                       "--file", str(path), "--overwrite", "false", "--if-none-match", "*",
                                       "--no-progress", "--timeout", "10", timeout=20)
                except AzureError as exc:
                    # Another controller can create and lease the blob between
                    # our existence check and conditional create.
                    if not any(code in str(exc) for code in ("BlobAlreadyExists", "ConditionNotMet", "LeaseIdMissing")):
                        raise
        acquired_at = time.monotonic()
        try:
            self._operation("acquire")
        except AzureError as exc:
            raise AzureError("Cannot acquire the allocation lease; another controller may be running") from exc
        self.renewed_at = acquired_at
        self.azure.lease = self
        self.worker = threading.Thread(target=self._renew, name="simdurl-allocation-lease", daemon=True)
        self.worker.start()
        return self

    def _renew(self):
        while not self.stop.wait(20):
            renewed_at = time.monotonic()
            try:
                self._operation("renew")
            except Exception:
                # Only a fixed error reaches user logs; credential diagnostics
                # and lease tokens do not cross from the renewal thread.
                self.failed.set()
                return
            self.renewed_at = renewed_at

    def check(self):
        if self.failed.is_set() or self.renewed_at is None or time.monotonic() - self.renewed_at >= 55:
            raise AzureError("Allocation lease renewal failed or expired; stopping this run for cleanup")

    def __exit__(self, exception_type, exception, traceback):
        self.stop.set()
        if self.worker is not None:
            self.worker.join(timeout=21)
        release_failed = False
        try:
            self._operation("release")
        except Exception:
            release_failed = True
            print("Allocation lease release failed; the lease expires automatically within 60 seconds", file=sys.stderr)
        finally:
            self.azure.lease = None
        # Preserve the original run/cleanup failure if there already is one.
        if exception_type is None:
            self.check()
            if release_failed:
                raise AzureError("Allocation lease release failed after cleanup")
        return False


def ensure_lease(azure):
    lease = getattr(azure, "lease", None)
    if lease is not None:
        lease.check()


def require_budget(azure):
    """Refuse new allocations at the configured reported monthly cost threshold.

    Azure metering is delayed. This early stop leaves headroom; it cannot impose
    an instantaneous billing cap. Cleanup never depends on budget availability.
    """
    policy = azure.config.get("budget")
    if policy is None:
        return
    resource_id = (f"/subscriptions/{azure.config['subscription_id']}"
                   f"/providers/Microsoft.Consumption/budgets/{policy['name']}")
    try:
        document = azure.call("rest", "--method", "get", "--url",
                              f"https://management.azure.com{resource_id}?api-version=2024-08-01",
                              timeout=30)
        properties = document["properties"]
        if (not isinstance(properties, dict) or properties.get("category") != "Cost" or properties.get("timeGrain") != "Monthly"
                or not finite_budget_number(properties.get("amount"))
                or properties["amount"] != policy["amount"]):
            raise ValueError("monthly cost budget amount or category differs from configuration")
        period = properties["timePeriod"]
        if not parse_time(period["startDate"]) <= now() < parse_time(period["endDate"]):
            raise ValueError("monthly budget is not active")
        expected_filter = {"dimensions": {"name": "ResourceGroupName", "operator": "In",
                                          "values": policy["resource_groups"]}}
        actual_filter = properties.get("filter")
        # Accept case/order differences in ARM group names, but no extra filter
        # that could silently exclude part of the benchmark's cost.
        if not isinstance(actual_filter, dict) or set(actual_filter) != {"dimensions"}:
            raise ValueError("budget must cover exactly the configured resource groups")
        dimension = actual_filter["dimensions"]
        if (not isinstance(dimension, dict) or set(dimension) != {"name", "operator", "values"}
                or dimension["name"] != expected_filter["dimensions"]["name"]
                or dimension["operator"] != "In"
                or not isinstance(dimension["values"], list)
                or len(dimension["values"]) != 2
                or any(not isinstance(group, str) for group in dimension["values"])
                or {group.lower() for group in dimension["values"]}
                != {group.lower() for group in policy["resource_groups"]}):
            raise ValueError("budget must cover exactly the configured resource groups")
        spend = properties["currentSpend"]
        if not isinstance(spend, dict):
            raise ValueError("budget spend is missing or invalid")
        amount = spend["amount"]
        if (spend.get("unit") != policy["currency"] or not finite_budget_number(amount) or amount < 0):
            raise ValueError("budget spend or currency is missing or invalid")
    except (AzureError, KeyError, TypeError, ValueError) as exc:
        raise AzureError(f"Monthly budget cannot be verified; refusing allocation: {exc}") from exc
    if amount >= policy["stop_at"]:
        raise AzureError(f"Monthly benchmark spend is {amount:.2f} {policy['currency']}; "
                         f"the allocation stop threshold is {policy['stop_at']:.2f}")


def require_ready(azure):
    require_budget(azure)
    heartbeat = azure.read_json("state", "heartbeat.json")
    age = now() - parse_time(heartbeat.get("updated_at"))
    if heartbeat.get("status") != "ok" or not timedelta(minutes=-1) <= age <= timedelta(minutes=15):
        raise AzureError("Azure cleanup timer is unhealthy or has no recent heartbeat; refusing allocation")
    # A workflow cancellation can release GitHub concurrency while leaving a VM.
    for blob in azure.storage("list", "--container-name", "runs", "--include", "m", "--num-results", "*"):
        if not blob["name"].endswith(".json"):
            continue
        # The controller checks outstanding runs. The independent reaper keeps
        # checking cleaned manifests for late resources until its expiry horizon.
        if (blob.get("metadata") or {}).get("status") == "cleaned":
            continue
        manifest = azure.read_json("runs", blob["name"])
        validate_manifest(manifest, azure.config["subscription_id"], azure.config["resource_group"])
        if manifest.get("status") == "cleaned":
            continue
        if manifest.get("status") == "registered" and parse_time(manifest["expires_at"]) > now():
            raise AzureError(f"Earlier allocation {manifest['run_id']} is registered and may still provision")
        pending = [key for key, resource_id in manifest["resource_ids"].items()
                   if azure.optional("resource", "show", "--ids", resource_id,
                                     "--api-version", RESOURCE_API_VERSIONS[key])]
        deployment = azure.optional("deployment", "group", "show", "--resource-group",
                                    azure.config["resource_group"], "--name", manifest["deployment_name"])
        active = deployment and deployment.get("properties", {}).get("provisioningState") in (
            "Accepted", "Running", "Creating")
        if pending or active:
            raise AzureError(f"Earlier allocation {manifest['run_id']} needs cleanup before another run")


def cleanup(azure, run_id, *, timeout=600):
    expected_resource_ids(run_id, azure.config["subscription_id"], azure.config["resource_group"])
    manifest = azure.read_json("runs", f"{run_id}.json", optional=True)
    if manifest is None:
        return
    validate_manifest(manifest, azure.config["subscription_id"], azure.config["resource_group"])
    manifest["status"] = "cleanup_pending"
    azure.write_json("runs", f"{run_id}.json", manifest)
    deadline = time.monotonic() + timeout
    errors = []
    empty_passes = 0
    while time.monotonic() < deadline:
        errors = []
        deployment = azure.optional("deployment", "group", "show", "--resource-group",
                                    azure.config["resource_group"], "--name", run_id)
        active = deployment and deployment.get("properties", {}).get("provisioningState") in (
            "Accepted", "Running", "Creating")
        if active:
            try:
                azure.call("deployment", "group", "cancel", "--resource-group",
                           azure.config["resource_group"], "--name", run_id)
            except AzureError as exc:
                errors.append(str(exc))
        present = False
        for kind in ("vm", "disk", "nic", "public_ip"):
            resource_id = manifest["resource_ids"][kind]
            resource = azure.optional("resource", "show", "--ids", resource_id,
                                      "--api-version", RESOURCE_API_VERSIONS[kind])
            if resource is None:
                continue
            present = True
            if not resource_is_owned(kind, resource, manifest):
                raise AzureError(f"Refusing to remove {kind}: resource ownership does not match {run_id}")
            try:
                azure.call("resource", "delete", "--ids", resource_id,
                           "--api-version", RESOURCE_API_VERSIONS[kind], "--no-wait")
            except AzureError as exc:
                errors.append(str(exc))
                if kind == "vm":
                    try:
                        azure.call("vm", "deallocate", "--ids", resource_id, "--no-wait")
                    except AzureError as fallback:
                        errors.append(str(fallback))
        empty_passes = empty_passes + 1 if not present and not active and not errors else 0
        if empty_passes >= 2:
            manifest["status"] = "cleaned"
            manifest["cleaned_at"] = timestamp(now())
            azure.write_json("runs", f"{run_id}.json", manifest)
            return
        time.sleep(min(10, max(0, deadline - time.monotonic())))
    # A stuck delete also needs the deallocation fallback, not just failed requests.
    try:
        azure.call("vm", "deallocate", "--ids", manifest["resource_ids"]["vm"], "--no-wait")
    except AzureError as exc:
        errors.append(str(exc))
    raise AzureError("Cleanup incomplete; independent Azure reaper will retry. " + "; ".join(errors))


def expected_cases():
    cases = {f"codec/{op}/{mode}/{pattern}/{length}/simdurl/automatic"
             for op, mode, pattern, length in benchmark.CORE_CASES["codec"]}
    cases |= {f"validate/{checks}/{pattern}/{length}/{variant}/automatic"
              for checks, pattern, length, variant in benchmark.CORE_CASES["validate"]}
    cases |= {f"formscan/{mode}/{pattern}/{length}/simdurl/automatic"
              for mode, pattern, length in benchmark.CORE_CASES["formscan"]}
    cases |= {f"helpers/{op}/{pattern}/{length}/{kind}/{variant}/automatic"
              for op, pattern, length, kind, variant in benchmark.CORE_CASES["helpers"]}
    return cases


def validate_result(directory, manifest):
    completion = json.loads((directory / "completion.json").read_text())
    if (completion.get("schema_version") != 1 or completion.get("exit_code") != 0
            or completion.get("commit") != manifest["commit"]
            or completion.get("image_id") != manifest["image_id"]):
        raise AzureError("Guest completion does not describe a successful run of the requested image/commit")
    if (not completion.get("boot_id_before")
            or completion["boot_id_before"] != completion.get("boot_id_after")):
        raise AzureError("VM restarted during measurement")
    for name in FILES[:-1]:
        data = (directory / name).read_bytes()
        descriptor = completion.get("files", {}).get(name, {})
        if descriptor.get("size") != len(data) or descriptor.get("sha256") != hashlib.sha256(data).hexdigest():
            raise AzureError(f"Evidence checksum mismatch: {name}")
    evidence = json.loads((directory / "evidence.json").read_text())["simdurl_evidence"]
    if (evidence.get("source_sha") != manifest["commit"] or evidence.get("exit_code") != 0
            or evidence.get("preflight", {}).get("status") != "passed"
            or evidence.get("preflight", {}).get("require_vbmi2") is not True
            or any(evidence.get("dispatch", {}).get(op) != "vbmi2" for op in ("encode", "decode"))):
        raise AzureError("Evidence lacks a successful required native VBMI2 preflight")
    native = evidence["preflight"].get("native_tests", {})
    if (type(native.get("exit_code")) is not int or native["exit_code"] != 0
            or not isinstance(native.get("stdout"), str)):
        raise AzureError("Native correctness tests did not complete successfully")
    spec = importlib.util.spec_from_file_location("simdurl_benchmark_gate", ROOT / "benchmarks/bencher/run.py")
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    try:
        records = gate.require_native_execution(native["stdout"])
    except gate.PreflightError as exc:
        raise AzureError(f"Invalid native execution evidence: {exc}") from exc
    if native.get("records") != records:
        raise AzureError("Native execution records differ from the captured test output")
    metadata = json.loads(evidence["files"]["metadata.json"])
    if metadata.get("status") != "complete" or metadata.get("commit") != manifest["commit"]:
        raise AzureError("Benchmark metadata is incomplete or belongs to another commit")
    if (metadata.get("suite") != "core" or type(metadata.get("benchmark_count")) is not int
            or metadata["benchmark_count"] != 10):
        raise AzureError("Azure measurements require the core suite with exactly ten benchmarks")
    if type(metadata.get("harness_version")) is not int or metadata["harness_version"] != 5:
        raise AzureError("Azure measurements require benchmark harness version 5")
    smoke = manifest.get("smoke") is True
    policy = {"mode": "fixed"} if smoke else benchmark.CALIBRATED_POLICY
    if metadata.get("sampling_policy") != policy:
        raise AzureError("Benchmark sampling policy does not match this run's measurement mode")
    expected_repeats = 5 if smoke else policy["samples_per_case"]
    codec_repeats = metadata.get("codec_repeats")
    if type(codec_repeats) is not int or codec_repeats != (1 if smoke else expected_repeats):
        raise AzureError("Codec repetition count does not match this run's measurement mode")
    if any(type(metadata.get(field)) is not int or metadata[field] != expected_repeats for field in
           ("validation_repeats", "formscan_repeats", "helper_repeats")):
        raise AzureError("Benchmark repetition counts do not match this run's measurement mode")
    samples = json.loads(evidence["files"]["samples.json"])
    if set(samples) != expected_cases():
        raise AzureError("Benchmark case set does not match the ten core benchmarks")
    for name, values in samples.items():
        if not isinstance(values, list) or len(values) != (codec_repeats if name.startswith("codec/") else expected_repeats):
            raise AzureError(f"Incorrect sample count for {name}")
        if any(isinstance(x, bool) or not isinstance(x, (int, float))
                             or not math.isfinite(x) or x <= 0 for x in values):
            raise AzureError("Invalid raw benchmark samples")
    if not smoke:
        try:
            measured_samples = benchmark.validate_calibrated_batches(metadata.get("batches"), expected_cases())
        except benchmark.BenchmarkError as exc:
            raise AzureError(f"Invalid calibrated sampling evidence: {exc}") from exc
        if measured_samples != samples:
            raise AzureError("Raw benchmark samples do not match the calibrated batch measurements")
    metrics = json.loads((directory / "results.json").read_text())
    if metrics != benchmark.bmf(samples):
        raise AzureError("Benchmark summary does not match raw samples")
    return metrics


def wait_deployment(azure, manifest, timeout=900):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ensure_lease(azure)
        result = azure.call("deployment", "group", "show", "--resource-group",
                            manifest["resource_group"], "--name", manifest["deployment_name"])
        state = result.get("properties", {}).get("provisioningState")
        if state == "Succeeded":
            return
        if state in ("Failed", "Canceled"):
            raise AzureError(f"VM deployment {state}; inspect deployment operations for details")
        time.sleep(10)
    raise AzureError("VM allocation exceeded 15 minutes")


def prepare_command(azure, manifest, input_hash, *, smoke=False):
    """Validate signing access and assemble protected inputs before allocating."""
    ensure_lease(azure)
    prefix = manifest["run_id"]
    expires = parse_time(manifest["expires_at"]) + timedelta(minutes=15)
    protected = {"SIMDURL_INPUT_URL": azure.sas("inputs", f"{prefix}/image.tar.gz", "r", expires)}
    for field, name in zip(("RESULTS", "EVIDENCE", "HOST", "COMPLETION"), FILES):
        protected[f"SIMDURL_{field}_URL"] = azure.sas("results", f"{prefix}/{name}", "cw", expires)
    parameters = {"SIMDURL_INPUT_SHA256": input_hash, "SIMDURL_IMAGE_ID": manifest["image_id"],
                  "SIMDURL_COMMIT": manifest["commit"], "SIMDURL_SMOKE": "1" if smoke else "0",
                  "SIMDURL_TIMEOUT": "900"}
    guest = (ROOT / "scripts/benchmark_azure_guest.py").read_text()
    return {"location": azure.config["location"], "properties": {
        "source": {"script": "#!/bin/bash\nset -eu\ncloud-init status --wait >/dev/null\n"
                   "python3 - <<'SIMDURL_GUEST_PY'\n" + guest + "\nSIMDURL_GUEST_PY\n"},
        "asyncExecution": True, "timeoutInSeconds": 1200,
        "parameters": [{"name": k, "value": v} for k, v in parameters.items()],
        "protectedParameters": [{"name": k, "value": v} for k, v in protected.items()],
        "outputBlobUri": azure.sas("results", f"{prefix}/host.stdout", "racw", expires),
        "errorBlobUri": azure.sas("results", f"{prefix}/host.stderr", "racw", expires)}}


def execute(azure, manifest, body):
    ensure_lease(azure)
    resource_id = manifest["resource_ids"]["vm"] + "/runCommands/benchmark"
    url = "https://management.azure.com" + resource_id + "?api-version=2024-07-01"
    with tempfile.TemporaryDirectory(prefix="simdurl-command-") as tmp:
        path = Path(tmp) / "protected.json"
        path.write_text(json.dumps(body))
        path.chmod(0o600)
        ensure_lease(azure)
        azure.call("rest", "--method", "put", "--url", url, "--body", f"@{path}")
    deadline = time.monotonic() + 1260
    while time.monotonic() < deadline:
        ensure_lease(azure)
        state = azure.call("rest", "--method", "get", "--url", url + "&$expand=instanceView")
        view = state.get("properties", {}).get("instanceView", {})
        if view.get("executionState") in ("Succeeded", "Failed", "TimedOut", "Canceled"):
            if view["executionState"] != "Succeeded" or view.get("exitCode") != 0:
                raise AzureError(f"Guest execution {view['executionState']} (exit {view.get('exitCode')})")
            return
        if state.get("properties", {}).get("provisioningState") == "Failed":
            raise AzureError("Managed Run Command provisioning failed")
        time.sleep(10)
    raise AzureError("Guest execution exceeded its controller deadline")


def run(azure, args):
    with AllocationLease(azure):
        return run_with_lease(azure, args)


def run_with_lease(azure, args):
    if not SHA.fullmatch(args.commit) or not SHA.fullmatch(args.harness_sha):
        raise ValueError("commit and harness-sha must be full lowercase commit SHAs")
    ids = expected_resource_ids(args.run_id, azure.config["subscription_id"], azure.config["resource_group"])
    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("output directory must be empty")
    image_id = command(["docker", "image", "inspect", "--format", "{{.Id}}", args.image]).decode().strip()
    if not IMAGE_ID.fullmatch(image_id):
        raise ValueError("Docker returned an invalid image content ID")
    ensure_lease(azure)
    require_ready(azure)
    manifest = None
    errors = []
    try:
        print(f"Preparing image for {args.run_id}", flush=True)
        with tempfile.TemporaryDirectory(prefix="simdurl-image-") as tmp:
            raw = Path(tmp) / "image.tar"
            bundle = Path(tmp) / "image.tar.gz"
            with raw.open("wb") as stream:
                command(["docker", "image", "save", args.image], stdout=stream, timeout=300)
            with raw.open("rb") as src, gzip.open(bundle, "wb", compresslevel=1) as dest:
                shutil.copyfileobj(src, dest)
            with bundle.open("rb") as stream:
                input_hash = hashlib.file_digest(stream, "sha256").hexdigest()
            azure.upload("inputs", f"{args.run_id}/image.tar.gz", bundle, overwrite=False)
        ensure_lease(azure)
        started = now()
        registered = {"schema_version": 1, "run_id": args.run_id,
                    "subscription_id": azure.config["subscription_id"],
                    "resource_group": azure.config["resource_group"], "resource_ids": ids,
                    "deployment_name": args.run_id, "created_at": timestamp(started),
                    "expires_at": timestamp(started + timedelta(minutes=azure.config["lifetime_minutes"])),
                    "status": "registered", "commit": args.commit, "harness_sha": args.harness_sha,
                    "image_id": image_id, "input_sha256": input_hash, "smoke": args.smoke,
                    "location": azure.config["location"], "vm_size": "Standard_D2s_v6"}
        validate_manifest(registered, azure.config["subscription_id"], azure.config["resource_group"])
        ensure_lease(azure)
        azure.write_json("runs", f"{args.run_id}.json", registered, overwrite=False)
        manifest = registered
        (output / "run.json").write_text(json.dumps(manifest, indent=2) + "\n")
        command_body = prepare_command(azure, manifest, input_hash, smoke=args.smoke)
        ensure_lease(azure)
        public_key = disposable_public_key()
        print(f"Allocating Standard_D2s_v6 in {azure.config['location']}", flush=True)
        with tempfile.TemporaryDirectory(prefix="simdurl-parameters-") as tmp:
            parameters = {"location": azure.config["location"], "runId": args.run_id,
                          "expiresAt": manifest["expires_at"], "adminPublicKey": public_key,
                          "imageVersion": azure.config["image_version"]}
            path = Path(tmp) / "parameters.json"
            path.write_text(json.dumps({"parameters": {k: {"value": v} for k, v in parameters.items()}}))
            ensure_lease(azure)
            azure.call("deployment", "group", "create", "--resource-group", manifest["resource_group"],
                       "--name", args.run_id, "--template-file", str(ROOT / "infra/azure/run.bicep"),
                       "--parameters", f"@{path}", "--no-wait")
        wait_deployment(azure, manifest)
        print("Running native VBMI2 checks and the benchmark suite", flush=True)
        execute(azure, manifest, command_body)
    except (AzureError, ValueError, OSError, KeyboardInterrupt) as exc:
        errors.append(str(exc) or "Interrupted")
    finally:
        if manifest is not None:
            print("Collecting measurement evidence", flush=True)
            collected = output / "guest"
            collected.mkdir(exist_ok=True)
            for name in (*FILES, "host.stdout", "host.stderr"):
                try:
                    azure.download("results", f"{args.run_id}/{name}", collected / name)
                except (AzureError, OSError) as exc:
                    if name in FILES:
                        errors.append(f"Could not retrieve {name}: {exc}")
            try:
                print("Deleting run resources", flush=True)
                cleanup(azure, args.run_id)
                (output / "cleanup.json").write_text('{"status":"cleaned"}\n')
            except (AzureError, ValueError, OSError) as exc:
                errors.append(f"Cleanup: {exc}")
            if not errors:
                try:
                    metrics = validate_result(collected, manifest)
                    (output / "results.json").write_text(json.dumps(metrics, sort_keys=True) + "\n")
                except (AzureError, ValueError, KeyError, OSError) as exc:
                    errors.append(f"Evidence validation: {exc}")
        (output / "status.json").write_text(json.dumps({"status": "failed" if errors else "complete",
                                                       "errors": errors}, indent=2) + "\n")
    if errors:
        raise AzureError("; ".join(errors))
    print(f"Complete: verified metrics in {output / 'results.json'}; run resources deleted", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("run", "cleanup", "check"))
    parser.add_argument("--config", required=True, help="JSON configuration or path to a JSON file")
    parser.add_argument("--run-id")
    parser.add_argument("--image")
    parser.add_argument("--commit")
    parser.add_argument("--harness-sha")
    parser.add_argument("--output-dir")
    parser.add_argument("--smoke", action="store_true", help="reduced iterations for local functionality tests")
    args = parser.parse_args(argv)
    required = ("run_id", "image", "commit", "harness_sha", "output_dir") if args.action == "run" else (
        ("run_id",) if args.action == "cleanup" else ())
    for field in required:
        if not getattr(args, field):
            parser.error(f"{args.action} requires --{field.replace('_', '-')}")
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"Interrupted by signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        azure = Azure(config_from(args.config, validate_budget=args.action != "cleanup"))
        if args.action == "run":
            run(azure, args)
        elif args.action == "cleanup":
            cleanup(azure, args.run_id)
        else:
            require_ready(azure)
        return 0
    except (AzureError, ValueError, OSError, KeyboardInterrupt) as exc:
        print(f"benchmark-azure: {redact(str(exc))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
