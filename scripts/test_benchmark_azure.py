"""Offline tests for the allocation gate, evidence import and cleanup."""

import copy
import contextlib
from datetime import datetime, timedelta, timezone
import hashlib
import itertools
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import benchmark_azure as azure

SUBSCRIPTION = "12345678-abcd-4321-abcd-123456789012"
COMMIT = "a" * 40
IMAGE = "sha256:" + "b" * 64


def configuration():
    return {
        "subscription_id": SUBSCRIPTION, "resource_group": "benchmark-compute",
        "location": "northeurope", "storage_account": "simdurlteststorage",
        "image_version": "24.04.202609010", "admin_public_key": "ssh-ed25519 AAAATEST",
    }


def run_manifest():
    config = configuration()
    run_id = "simdurl-123-1-abcdef12"
    created = datetime.now(timezone.utc)
    return {
        "schema_version": 1, "run_id": run_id, "subscription_id": SUBSCRIPTION,
        "resource_group": config["resource_group"], "deployment_name": run_id,
        "created_at": azure.timestamp(created), "expires_at": azure.timestamp(created + timedelta(hours=1)),
        "resource_ids": azure.expected_resource_ids(run_id, SUBSCRIPTION, config["resource_group"]),
        "status": "registered", "commit": COMMIT, "image_id": IMAGE, "smoke": False,
    }


def evidence_fixture():
    records = [{"operation": op, "backend": "vbmi2", "compiled": "1", "executed": "1",
                "skipped": "0", "cases": "20", "kernel_calls": "40"} for op in ("encode", "decode")]
    stdout = "\n".join("SIMDURL_BACKEND " + " ".join(f"{key}={value}" for key, value in record.items())
                       for record in records)
    samples = {name: [1.0, 2.0, 3.0, 4.0, 5.0] for name in azure.expected_cases()}
    metadata = {"status": "complete", "commit": COMMIT, "suite": "core", "benchmark_count": 10, "codec_repeats": 5,
                "validation_repeats": 5, "formscan_repeats": 5, "helper_repeats": 5}
    evidence = {"source_sha": COMMIT, "exit_code": 0, "dispatch": {"encode": "vbmi2", "decode": "vbmi2"},
                "preflight": {"status": "passed", "require_vbmi2": True,
                              "native_tests": {"exit_code": 0, "stdout": stdout, "records": records}},
                "files": {"metadata.json": json.dumps(metadata), "samples.json": json.dumps(samples)}}
    return evidence, samples


def write_result(directory, evidence, samples, *, boot_after="boot-id"):
    data = {"evidence.json": {"simdurl_evidence": evidence}, "host.json": {},
            "results.json": azure.benchmark.bmf(samples)}
    descriptors = {}
    for name, document in data.items():
        raw = json.dumps(document).encode()
        (directory / name).write_bytes(raw)
        descriptors[name] = {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    completion = {"schema_version": 1, "exit_code": 0, "commit": COMMIT, "image_id": IMAGE,
                  "boot_id_before": "boot-id", "boot_id_after": boot_after, "files": descriptors}
    (directory / "completion.json").write_text(json.dumps(completion))


class ConfigTests(unittest.TestCase):
    def test_default_lifetime_and_pinned_image(self):
        self.assertEqual(azure.config_from(json.dumps(configuration()))["lifetime_minutes"], 60)
        for changes in ({"image_version": "latest"}, {"lifetime_minutes": 45}, {"lifetime_minutes": 121},
                        {"lifetime_minutes": True}, {"storage_account": "invalid/account"}):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    azure.config_from(json.dumps(configuration() | changes))

    def test_redacts_signed_urls(self):
        self.assertNotIn("sig=private", azure.redact("download https://x.blob.core.windows.net/a/b?sig=private failed"))

    def test_sas_expiry_uses_utc_whole_seconds_for_azure_cli(self):
        client = azure.Azure(configuration())
        expires = datetime(2026, 9, 23, 14, 15, 16, 123456, tzinfo=timezone(timedelta(hours=1)))
        with mock.patch.object(client, "storage", return_value="test-token") as storage:
            result = client.sas("inputs", "fixture.tar.gz", "r", expires)
        arguments = storage.call_args.args
        self.assertEqual(arguments[arguments.index("--expiry") + 1], "2026-09-23T13:15:16Z")
        self.assertTrue(result.endswith("/inputs/fixture.tar.gz?test-token"))

    def test_manifest_status_metadata_is_uploaded_with_matching_document(self):
        client = azure.Azure(configuration())
        documents = []

        def capture(*arguments, **kwargs):
            document = json.loads(Path(arguments[arguments.index("--file") + 1]).read_text())
            metadata = arguments[arguments.index("--metadata") + 1]
            documents.append((document["status"], metadata))

        with mock.patch.object(client, "storage", side_effect=capture):
            for status in ("registered", "cleanup_pending", "cleaned"):
                client.write_json("runs", "fixture.json", {"status": status})
        self.assertEqual(documents, [(status, f"status={status}") for status in
                                     ("registered", "cleanup_pending", "cleaned")])


class BudgetTests(unittest.TestCase):
    def setUp(self):
        self.policy = {"name": "simdurl-benchmark-monthly", "amount": 100, "stop_at": 80,
                       "currency": "USD", "resource_groups": ["benchmark-compute", "benchmark-control"]}
        self.client = mock.Mock(spec=azure.Azure)
        self.config = configuration() | {"control_resource_group": "benchmark-control", "budget": self.policy}
        self.client.config = azure.config_from(json.dumps(self.config))
        self.properties = {
            "category": "Cost", "timeGrain": "Monthly", "amount": 100,
            "timePeriod": {"startDate": "2020-01-01T00:00:00Z", "endDate": "2100-01-01T00:00:00Z"},
            "currentSpend": {"amount": 0, "unit": "USD"},
            "filter": {"dimensions": {"name": "ResourceGroupName", "operator": "In",
                                       "values": ["BENCHMARK-CONTROL", "BENCHMARK-COMPUTE"]}},
        }
        self.client.call.return_value = {"properties": self.properties}

    def test_valid_budget_allows_reported_spend_below_threshold(self):
        for cost in (0, 79.99):
            self.properties["currentSpend"]["amount"] = cost
            azure.require_budget(self.client)
        args = self.client.call.call_args.args
        self.assertIn(f"/subscriptions/{SUBSCRIPTION}/providers/Microsoft.Consumption/budgets/{self.policy['name']}", args[-1])
        self.assertEqual(self.client.call.call_args.kwargs["timeout"], 30)

    def test_threshold_blocks_readiness_before_other_cloud_operations(self):
        for cost in (80, 100, 1000):
            self.properties["currentSpend"]["amount"] = cost
            with self.subTest(cost=cost), self.assertRaisesRegex(azure.AzureError, "stop threshold"):
                azure.require_ready(self.client)
        self.client.read_json.assert_not_called()
        self.client.storage.assert_not_called()

    def test_failed_budget_lookup_refuses_allocation(self):
        self.client.call.side_effect = azure.AzureError("budget unavailable")
        with self.assertRaisesRegex(azure.AzureError, "cannot be verified"):
            azure.require_ready(self.client)
        self.client.read_json.assert_not_called()

    def test_missing_invalid_or_wrong_currency_spend_is_rejected(self):
        for spend in (None, {}, {"amount": 0, "unit": "GBP"},
                      {"amount": True, "unit": "USD"}, {"amount": -1, "unit": "USD"},
                      {"amount": float("nan"), "unit": "USD"}, {"amount": "1", "unit": "USD"},
                      {"amount": 10 ** 1000, "unit": "USD"}):
            self.properties["currentSpend"] = spend
            with self.subTest(spend=spend), self.assertRaises(azure.AzureError):
                azure.require_budget(self.client)

    def test_altered_budget_period_amount_or_scope_is_rejected(self):
        original = copy.deepcopy(self.properties)
        for changes in ({"timeGrain": "Annually"}, {"amount": 1000}, {"amount": 10 ** 1000}, {"category": "Usage"},
                        {"timePeriod": {"startDate": "2020-01-01T00:00:00Z", "endDate": "2020-02-01T00:00:00Z"}},
                        {"filter": {}}, {"filter": {"dimensions": {"name": "ResourceGroupName", "operator": "In", "values": ["benchmark-compute"]}}},
                        {"filter": original["filter"] | {"tags": {"name": "service", "operator": "In", "values": ["vm"]}}}):
            self.client.call.return_value = {"properties": original | changes}
            with self.subTest(changes=changes), self.assertRaises(azure.AzureError):
                azure.require_budget(self.client)

    def test_budget_configuration_requires_headroom_and_both_groups(self):
        for changes in ({"stop_at": 100}, {"stop_at": True}, {"stop_at": 0}, {"amount": float("inf")},
                        {"amount": 10 ** 1000}, {"stop_at": 10 ** 1000},
                        {"name": "bad/name"}, {"currency": "usd"}, {"resource_groups": ["benchmark-compute"]},
                        {"resource_groups": ["benchmark-compute", "BENCHMARK-COMPUTE"]},
                        {"resource_groups": ["benchmark-compute", "unrelated-group"]}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                azure.config_from(json.dumps(self.config | {"budget": self.policy | changes}))

    def test_budget_requires_valid_distinct_control_group(self):
        missing = self.config.copy()
        del missing["control_resource_group"]
        for config in (missing, *(self.config | {"control_resource_group": group}
                                 for group in (None, "", 123, "bad/group", "BENCHMARK-COMPUTE"))):
            with self.subTest(config=config), self.assertRaises(ValueError):
                azure.config_from(json.dumps(config))

    def test_budget_group_comparison_allows_case_and_order_differences(self):
        config = self.config | {"control_resource_group": "BENCHMARK-CONTROL",
                              "budget": self.policy | {"resource_groups": ["benchmark-control", "BENCHMARK-COMPUTE"]}}
        self.assertEqual(azure.config_from(json.dumps(config))["budget"], config["budget"])

    def test_unconfigured_budget_preserves_other_deployments(self):
        self.client.config = configuration()
        azure.require_budget(self.client)
        self.client.call.assert_not_called()

    @mock.patch.object(azure.time, "sleep")
    def test_cleanup_does_not_read_budget_even_when_lookup_would_fail(self, sleep):
        client = FakeAzure(kinds=tuple(azure.RESOURCE_API_VERSIONS))
        client.config = self.client.config
        expected_resources = set(client.resources)
        original_call = client.call

        def call(*args, **kwargs):
            if args[:1] == ("rest",):
                raise azure.AzureError("budget unavailable")
            return original_call(*args, **kwargs)

        with mock.patch.object(client, "call", side_effect=call) as cloud_call:
            azure.cleanup(client, client.record["run_id"])
        self.assertFalse(any(item.args[:1] == ("rest",) for item in cloud_call.call_args_list))
        deleted = {args[args.index("--ids") + 1] for args in client.calls if args[:2] == ("resource", "delete")}
        self.assertEqual(deleted, expected_resources)
        self.assertEqual(client.resources, {})
        self.assertEqual(client.record["status"], "cleaned")

    @mock.patch.object(azure.time, "sleep")
    @mock.patch.object(azure.signal, "signal")
    def test_cleanup_command_accepts_malformed_budget_configuration(self, signal, sleep):
        malformed = self.config | {"budget": None, "control_resource_group": "bad/group"}
        with self.assertRaises(ValueError):
            azure.config_from(json.dumps(malformed))
        with self.assertRaises(ValueError):
            azure.config_from(json.dumps(malformed | {"resource_group": "bad/group"}), validate_budget=False)

        client = FakeAzure(kinds=tuple(azure.RESOURCE_API_VERSIONS))

        def create_client(config):
            client.config = config
            return client

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "config.json"
            path.write_text(json.dumps(malformed))
            with mock.patch.object(azure, "Azure", side_effect=create_client):
                result = azure.main(["cleanup", "--config", str(path), "--run-id", client.record["run_id"]])
        self.assertEqual(result, 0)
        self.assertEqual(client.resources, {})
        self.assertEqual(client.record["status"], "cleaned")


class LeaseStore:
    def __init__(self):
        self.exists = False
        self.holder = None
        self.fail_renew = False
        self.fail_release = False
        self.calls = []

    def storage(self, *args, **kwargs):
        self.calls.append((args, kwargs))
        if args[0] == "exists":
            return {"exists": self.exists}
        if args[0] == "upload":
            if self.exists:
                raise azure.AzureError("BlobAlreadyExists")
            self.exists = True
            return {}
        operation = args[1]
        if operation == "acquire":
            if self.holder is not None:
                raise azure.AzureError("LeaseAlreadyPresent")
            self.holder = args[args.index("--proposed-lease-id") + 1]
            return self.holder
        lease_id = args[args.index("--lease-id") + 1]
        if lease_id != self.holder:
            raise azure.AzureError("LeaseIdMismatchWithLeaseOperation")
        if operation == "renew":
            if self.fail_renew:
                raise azure.AzureError("temporary network failure")
            return self.holder
        if operation == "release":
            if self.fail_release:
                raise azure.AzureError("temporary network failure")
            self.holder = None
            return None
        raise AssertionError(args)


class LeaseTests(unittest.TestCase):
    def test_competing_controllers_cannot_acquire_and_release_allows_next_run(self):
        store = LeaseStore()
        first = mock.Mock(spec=azure.Azure)
        first.storage = store.storage
        second = mock.Mock(spec=azure.Azure)
        second.storage = store.storage
        with azure.AllocationLease(first):
            azure.ensure_lease(first)
            with self.assertRaisesRegex(azure.AzureError, "another controller"):
                with azure.AllocationLease(second):
                    self.fail("Competing controller acquired the same lease")
        self.assertIsNone(store.holder)
        with azure.AllocationLease(second):
            azure.ensure_lease(second)
        for arguments, options in store.calls:
            self.assertEqual(options["timeout"], 20)
            if arguments[0] == "lease":
                self.assertIn("--blob-name", arguments)
        acquired = [arguments for arguments, _ in store.calls if arguments[:2] == ("lease", "acquire")]
        self.assertTrue(all(arguments[arguments.index("--lease-duration") + 1] == "60" for arguments in acquired))

    def test_renewal_failure_blocks_work_and_cleanup_remains_available(self):
        store = LeaseStore()
        client = mock.Mock(spec=azure.Azure)
        client.storage = store.storage
        lease = azure.AllocationLease(client)
        store.holder = lease.lease_id
        store.fail_renew = True
        lease.renewed_at = azure.time.monotonic()
        client.lease = lease
        with mock.patch.object(lease.stop, "wait", return_value=False):
            lease._renew()
        with self.assertRaisesRegex(azure.AzureError, "renewal failed"):
            azure.ensure_lease(client)
        with self.assertRaisesRegex(azure.AzureError, "renewal failed"):
            lease.__exit__(None, None, None)
        self.assertIsNone(store.holder)
        self.assertIsNone(client.lease)

    def test_lease_expiry_detected_even_if_renewal_thread_was_not_scheduled(self):
        client = mock.Mock(spec=azure.Azure)
        lease = azure.AllocationLease(client)
        lease.renewed_at = azure.time.monotonic() - 56
        with self.assertRaisesRegex(azure.AzureError, "expired"):
            lease.check()

    def test_release_failure_does_not_hide_original_run_failure(self):
        store = LeaseStore()
        store.fail_release = True
        client = mock.Mock(spec=azure.Azure)
        client.storage = store.storage
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "original failure"):
                with azure.AllocationLease(client):
                    raise ValueError("original failure")
        self.assertIsNone(client.lease)

    def test_failed_lease_stops_deployment_polling_before_cloud_call(self):
        client = mock.Mock(spec=azure.Azure)
        client.lease = mock.Mock()
        client.lease.check.side_effect = azure.AzureError("lease lost")
        with self.assertRaisesRegex(azure.AzureError, "lease lost"):
            azure.wait_deployment(client, run_manifest())
        client.call.assert_not_called()

    def test_lease_lost_before_allocation_still_cleans_registered_run(self):
        client = mock.Mock(spec=azure.Azure)
        client.config = configuration() | {"lifetime_minutes": 60}
        client.lease = mock.Mock()
        client.lease.check.side_effect = [None, None, None, azure.AzureError("lease lost")]
        client.download.side_effect = azure.AzureError("no results before allocation")

        def docker(arguments, *, stdout=None, **kwargs):
            if "inspect" in arguments:
                return IMAGE.encode()
            stdout.write(b"image fixture")
            return b""

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(azure, "command", side_effect=docker), \
                mock.patch.object(azure, "require_ready"), \
                mock.patch.object(azure, "cleanup") as cleanup:
            args = SimpleNamespace(commit=COMMIT, harness_sha=COMMIT, run_id=run_manifest()["run_id"],
                                   output_dir=temporary, image="fixture:local", smoke=False)
            with self.assertRaisesRegex(azure.AzureError, "lease lost"):
                azure.run_with_lease(client, args)
            cleanup.assert_called_once_with(client, args.run_id)
            client.call.assert_not_called()


class CommandPreparationTests(unittest.TestCase):
    def test_preparation_signs_all_guest_urls_without_allocating(self):
        client = mock.Mock(spec=azure.Azure)
        client.config = configuration()
        client.sas.return_value = "https://storage.example/blob?test-token"
        body = azure.prepare_command(client, run_manifest(), "c" * 64, smoke=True)
        self.assertEqual(client.sas.call_count, 7)
        self.assertEqual(len(body["properties"]["protectedParameters"]), 5)
        parameters = {item["name"]: item["value"] for item in body["properties"]["parameters"]}
        self.assertEqual(parameters["SIMDURL_SMOKE"], "1")
        client.call.assert_not_called()

    def test_signing_failure_precedes_compute_allocation(self):
        client = mock.Mock(spec=azure.Azure)
        client.config = configuration() | {"lifetime_minutes": 60}
        client.sas.side_effect = azure.AzureError("signing unavailable")
        client.download.side_effect = azure.AzureError("no results before allocation")

        def docker(arguments, *, stdout=None, **kwargs):
            if "inspect" in arguments:
                return IMAGE.encode()
            stdout.write(b"image fixture")
            return b""

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(azure, "command", side_effect=docker), \
                mock.patch.object(azure, "require_ready"), \
                mock.patch.object(azure, "cleanup") as cleanup:
            args = SimpleNamespace(commit=COMMIT, harness_sha=COMMIT, run_id=run_manifest()["run_id"],
                                   output_dir=temporary, image="fixture:local", smoke=False)
            with self.assertRaisesRegex(azure.AzureError, "signing unavailable"):
                azure.run_with_lease(client, args)
            cleanup.assert_called_once_with(client, args.run_id)
            client.call.assert_not_called()


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.evidence, self.samples = evidence_fixture()
        self.record = run_manifest()

    def test_complete_evidence_imports_exactly_the_ten_core_cases(self):
        write_result(self.directory, self.evidence, self.samples)
        result = azure.validate_result(self.directory, self.record)
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

    def test_full_or_unspecified_suite_and_wrong_benchmark_count_are_rejected(self):
        metadata = json.loads(self.evidence["files"]["metadata.json"])
        for changes in ({"suite": "full", "benchmark_count": 1356}, {"suite": "full"}, {"suite": None},
                        {"benchmark_count": None}, {"benchmark_count": 9}, {"benchmark_count": 11},
                        {"benchmark_count": 10.0}, {"benchmark_count": True}):
            with self.subTest(changes=changes):
                self.evidence["files"]["metadata.json"] = json.dumps(metadata | changes)
                write_result(self.directory, self.evidence, self.samples)
                with self.assertRaisesRegex(azure.AzureError, "core suite"):
                    azure.validate_result(self.directory, self.record)

    def test_noncore_and_scalar_cases_are_rejected_even_with_core_metadata(self):
        for name in ("codec/encode/URI/mixed/128/simdurl/scalar",
                     "codec/encode/URI/mixed/16/simdurl/automatic"):
            with self.subTest(name=name):
                samples = self.samples | {name: [1.0, 2.0, 3.0, 4.0, 5.0]}
                self.evidence["files"]["samples.json"] = json.dumps(samples)
                write_result(self.directory, self.evidence, samples)
                with self.assertRaisesRegex(azure.AzureError, "case set"):
                    azure.validate_result(self.directory, self.record)

    def test_changed_file_and_reboot_are_rejected(self):
        write_result(self.directory, self.evidence, self.samples)
        (self.directory / "host.json").write_text("changed")
        with self.assertRaisesRegex(azure.AzureError, "checksum"):
            azure.validate_result(self.directory, self.record)
        write_result(self.directory, self.evidence, self.samples, boot_after="different-boot")
        with self.assertRaisesRegex(azure.AzureError, "restarted"):
            azure.validate_result(self.directory, self.record)

    def test_native_gate_cannot_be_claimed_without_execution(self):
        native = self.evidence["preflight"]["native_tests"]
        for changes in ({"exit_code": 1}, {"stdout": ""}, {"records": []},
                        {"stdout": native["stdout"].replace("kernel_calls=40", "kernel_calls=0")}):
            with self.subTest(changes=changes):
                evidence = copy.deepcopy(self.evidence)
                evidence["preflight"]["native_tests"].update(changes)
                write_result(self.directory, evidence, self.samples)
                with self.assertRaises(azure.AzureError):
                    azure.validate_result(self.directory, self.record)

    def test_dispatch_and_preflight_status_checked(self):
        self.evidence["dispatch"]["encode"] = "avx2"
        write_result(self.directory, self.evidence, self.samples)
        with self.assertRaisesRegex(azure.AzureError, "preflight"):
            azure.validate_result(self.directory, self.record)

    def test_missing_case_and_missing_sample_are_rejected(self):
        missing = dict(self.samples)
        missing.pop(next(iter(missing)))
        for samples in (missing, self.samples | {next(iter(self.samples)): [1.0]}):
            with self.subTest(cases=len(samples)):
                self.evidence["files"]["samples.json"] = json.dumps(samples)
                write_result(self.directory, self.evidence, samples)
                with self.assertRaises(azure.AzureError):
                    azure.validate_result(self.directory, self.record)

    def test_smoke_changes_only_codec_repetitions(self):
        samples = {name: values[:1] if name.startswith("codec/") else values for name, values in self.samples.items()}
        metadata = json.loads(self.evidence["files"]["metadata.json"])
        metadata["codec_repeats"] = 1
        self.evidence["files"].update({"samples.json": json.dumps(samples), "metadata.json": json.dumps(metadata)})
        write_result(self.directory, self.evidence, samples)
        with self.assertRaisesRegex(azure.AzureError, "repetition"):
            azure.validate_result(self.directory, self.record)
        self.record["smoke"] = True
        self.assertEqual(len(azure.validate_result(self.directory, self.record)), 10)


class FakeAzure:
    def __init__(self, record=None, kinds=()):
        self.config = configuration()
        self.record = copy.deepcopy(record or run_manifest())
        self.resources = {self.record["resource_ids"][kind]: {
            "id": self.record["resource_ids"][kind],
            "tags": {"simdurl-owner": "simdurl", "simdurl-run": self.record["run_id"]},
        } for kind in kinds}
        self.heartbeat = {"status": "ok", "updated_at": azure.timestamp(datetime.now(timezone.utc))}
        self.calls = []
        self.stuck = False
        self.deployment_state = "Succeeded"

    def read_json(self, container, name, **kwargs):
        return copy.deepcopy(self.heartbeat if container == "state" else self.record)

    def write_json(self, container, name, data, **kwargs):
        self.record = copy.deepcopy(data)

    def storage(self, *args):
        return [{"name": self.record["run_id"] + ".json"}]

    def optional(self, *args):
        self.calls.append(args)
        if args[:2] == ("deployment", "group"):
            return {"properties": {"provisioningState": self.deployment_state}}
        if args[:2] == ("resource", "show"):
            return self.resources.get(args[args.index("--ids") + 1])
        raise AssertionError(args)

    def call(self, *args, **kwargs):
        self.calls.append(args)
        if args[:2] == ("resource", "delete") and not self.stuck:
            self.resources.pop(args[args.index("--ids") + 1], None)


class LifecycleTests(unittest.TestCase):
    def test_cleaned_metadata_skips_historical_manifest_download(self):
        client = FakeAzure()
        client.storage = mock.Mock(return_value=[{"name": "historical.json", "metadata": {"status": "cleaned"}}])
        client.read_json = mock.Mock(wraps=client.read_json)
        azure.require_ready(client)
        client.read_json.assert_called_once_with("state", "heartbeat.json")
        client.storage.assert_called_once_with("list", "--container-name", "runs", "--include", "m", "--num-results", "*")

    def test_old_manifests_without_metadata_are_still_validated(self):
        client = FakeAzure(run_manifest() | {"status": "cleaned"})
        client.read_json = mock.Mock(wraps=client.read_json)
        azure.require_ready(client)
        self.assertEqual(client.read_json.call_count, 2)
        client.record["resource_group"] = "unrelated"
        with self.assertRaises(ValueError):
            azure.require_ready(client)

    def test_noncleaned_metadata_does_not_bypass_manifest_validation(self):
        client = FakeAzure()
        client.storage = mock.Mock(return_value=[{"name": "fixture.json", "metadata": {"status": "registered"}}])
        client.record["resource_ids"]["disk"] = "unrelated-disk"
        with self.assertRaises(ValueError):
            azure.require_ready(client)

    def test_allocation_requires_healthy_recent_reaper(self):
        client = FakeAzure()
        client.heartbeat["updated_at"] = azure.timestamp(datetime.now(timezone.utc) - timedelta(minutes=20))
        with self.assertRaisesRegex(azure.AzureError, "timer"):
            azure.require_ready(client)

    def test_registered_run_blocks_allocation_before_resource_exists(self):
        client = FakeAzure()
        with self.assertRaisesRegex(azure.AzureError, "registered"):
            azure.require_ready(client)

    def test_remaining_resources_block_next_allocation(self):
        record = run_manifest() | {"status": "cleanup_pending"}
        client = FakeAzure(record, ("disk",))
        with self.assertRaisesRegex(azure.AzureError, "needs cleanup"):
            azure.require_ready(client)

    @mock.patch.object(azure.time, "sleep")
    def test_cleanup_removes_partial_allocation_and_verifies_absence(self, sleep):
        client = FakeAzure(kinds=("disk", "nic", "public_ip"))
        azure.cleanup(client, client.record["run_id"])
        self.assertEqual(client.resources, {})
        self.assertEqual(client.record["status"], "cleaned")
        self.assertGreaterEqual(sleep.call_count, 2)
        for call in client.calls:
            if call[0] == "resource":
                self.assertIn("--api-version", call)

    @mock.patch.object(azure.time, "sleep")
    def test_cleanup_refuses_foreign_ownership(self, sleep):
        client = FakeAzure(kinds=("vm",))
        next(iter(client.resources.values()))["tags"]["simdurl-run"] = "other-run"
        with self.assertRaisesRegex(azure.AzureError, "ownership"):
            azure.cleanup(client, client.record["run_id"])
        self.assertFalse(any(call[:2] == ("resource", "delete") for call in client.calls))

    @mock.patch.object(azure.time, "sleep")
    def test_stuck_delete_deallocates_and_reports_pending(self, sleep):
        client = FakeAzure(kinds=("vm",))
        client.stuck = True
        with mock.patch.object(azure.time, "monotonic", side_effect=itertools.count()):
            with self.assertRaisesRegex(azure.AzureError, "Cleanup incomplete"):
                azure.cleanup(client, client.record["run_id"], timeout=5)
        self.assertTrue(any(call[:2] == ("vm", "deallocate") for call in client.calls))
        self.assertEqual(client.record["status"], "cleanup_pending")


if __name__ == "__main__":
    unittest.main()
