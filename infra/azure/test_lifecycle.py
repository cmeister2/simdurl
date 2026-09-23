"""Offline lifecycle tests: no Azure credentials or resources are used."""

import copy
from datetime import timedelta
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "function_app"))
from lifecycle import expected_resource_ids, parse_time, resource_is_owned, validate_manifest
from reaper import sweep

SUBSCRIPTION = "12345678-abcd-4321-abcd-123456789012"
GROUP = "benchmark-compute"


def manifest():
    run_id = "simdurl-123-1-abcdef12"
    return {
        "schema_version": 1,
        "run_id": run_id,
        "subscription_id": SUBSCRIPTION,
        "resource_group": GROUP,
        "resource_ids": expected_resource_ids(run_id, SUBSCRIPTION, GROUP),
        "deployment_name": run_id,
        "created_at": "2026-09-23T12:00:00Z",
        "expires_at": "2026-09-23T13:00:00Z",
    }


def resource(record, kind, tagged=True):
    result = {
        "id": record["resource_ids"][kind],
        "tags": {"simdurl-owner": "simdurl", "simdurl-run": record["run_id"]} if tagged else {},
    }
    if kind == "disk":
        result.update(managedBy=record["resource_ids"]["vm"], properties={"timeCreated": record["created_at"]})
    return result


class FakeArm:
    def __init__(self, record, kinds=(), deployment_state="Succeeded"):
        self.resources = {record["resource_ids"][kind]: resource(record, kind, tagged=kind != "disk") for kind in kinds}
        self.deployment_state = deployment_state
        self.calls = []
        self.delete_failure = False

    def get(self, resource_id):
        if "/deployments/" in resource_id:
            return {"properties": {"provisioningState": self.deployment_state}}
        return copy.deepcopy(self.resources.get(resource_id))

    def delete(self, resource_id):
        self.calls.append(("DELETE", resource_id))
        if self.delete_failure:
            raise RuntimeError("delete denied")
        # Deliberately leave the resource present to model an accepted async
        # deletion; callers must not treat this return value as completion.

    def request(self, method, resource_id, action=""):
        self.calls.append((method, resource_id + action))


class ManifestTests(unittest.TestCase):
    def test_valid_manifest(self):
        validate_manifest(manifest(), SUBSCRIPTION, GROUP)

    def test_arbitrary_resource_rejected(self):
        record = manifest()
        record["resource_ids"]["nic"] = record["resource_ids"]["nic"].replace(record["run_id"], "unrelated")
        with self.assertRaises(ValueError):
            validate_manifest(record, SUBSCRIPTION, GROUP)

    def test_wrong_scope_and_deployment_rejected(self):
        for key, value in (("resource_group", "control"), ("deployment_name", "shared-infrastructure")):
            with self.subTest(key=key):
                record = manifest()
                record[key] = value
                with self.assertRaises(ValueError):
                    validate_manifest(record, SUBSCRIPTION, GROUP)

    def test_bad_expiry_rejected(self):
        for expires in ("2026-09-23T15:00:00Z", "2026-09-23T11:00:00Z", "2026-09-23T13:00:00"):
            with self.subTest(expires=expires):
                record = manifest()
                record["expires_at"] = expires
                with self.assertRaises(ValueError):
                    validate_manifest(record, SUBSCRIPTION, GROUP)

    def test_untagged_implicit_disk_is_owned(self):
        record = manifest()
        self.assertTrue(resource_is_owned("disk", resource(record, "disk", tagged=False), record))
        self.assertFalse(resource_is_owned("vm", resource(record, "vm", tagged=False), record))

    def test_untagged_disk_must_be_recent_and_not_attached_elsewhere(self):
        record = manifest()
        disk = resource(record, "disk", tagged=False)
        disk["managedBy"] = "another-vm"
        self.assertFalse(resource_is_owned("disk", disk, record))
        disk["managedBy"] = ""
        disk["properties"]["timeCreated"] = "2026-09-22T12:00:00Z"
        self.assertFalse(resource_is_owned("disk", disk, record))


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.record = manifest()
        self.now = parse_time(self.record["expires_at"])

    def test_async_vm_delete_stays_pending_without_touching_dependencies(self):
        arm = FakeArm(self.record, ("vm", "disk", "nic", "public_ip"))
        state = sweep(arm, self.record, {}, self.now)
        self.assertEqual(arm.calls, [("DELETE", self.record["resource_ids"]["vm"])])
        self.assertEqual(state["status"], "cleanup_pending")
        self.assertEqual(len(state["remaining"]), 4)

    def test_partial_allocation_cleans_disk_nic_then_ip(self):
        arm = FakeArm(self.record, ("disk", "nic", "public_ip"))
        sweep(arm, self.record, {}, self.now)
        self.assertEqual(arm.calls, [("DELETE", self.record["resource_ids"][kind]) for kind in ("disk", "nic")])
        arm.resources.pop(self.record["resource_ids"]["disk"])
        arm.resources.pop(self.record["resource_ids"]["nic"])
        arm.calls.clear()
        sweep(arm, self.record, {}, self.now)
        self.assertEqual(arm.calls, [("DELETE", self.record["resource_ids"]["public_ip"])])

    def test_cancel_active_deployment_even_when_nothing_exists_yet(self):
        arm = FakeArm(self.record, deployment_state="Running")
        state = sweep(arm, self.record, {}, self.now + timedelta(hours=2))
        self.assertEqual(arm.calls[0][0], "POST")
        self.assertTrue(arm.calls[0][1].endswith("/cancel"))
        self.assertFalse(state["deployment_terminal"])
        self.assertEqual(state["status"], "cleanup_pending")

    def test_delete_failure_attempts_deallocation(self):
        arm = FakeArm(self.record, ("vm",))
        arm.delete_failure = True
        state = sweep(arm, self.record, {}, self.now)
        self.assertTrue(arm.calls[-1][1].endswith("/deallocate"))
        self.assertEqual(state["errors"], ["delete denied"])

    def test_stuck_accepted_delete_also_attempts_deallocation(self):
        arm = FakeArm(self.record, ("vm",))
        previous = sweep(arm, self.record, {}, self.now)
        sweep(arm, self.record, previous, self.now + timedelta(minutes=10))
        self.assertTrue(arm.calls[-1][1].endswith("/deallocate"))

    def test_ownership_mismatch_never_deletes(self):
        arm = FakeArm(self.record, ("vm", "nic"))
        arm.resources[self.record["resource_ids"]["nic"]]["tags"]["simdurl-run"] = "another-run"
        state = sweep(arm, self.record, {}, self.now)
        self.assertEqual(arm.calls, [])
        self.assertIn("ownership mismatch", state["errors"][0])

    def test_tombstone_waits_for_late_resources_and_empty_rechecks(self):
        arm = FakeArm(self.record)
        first = sweep(arm, self.record, {}, self.now)
        second = sweep(arm, self.record, first, self.now + timedelta(minutes=5))
        self.assertEqual(second["status"], "cleanup_pending")
        arm.resources[self.record["resource_ids"]["disk"]] = resource(self.record, "disk", tagged=False)
        third = sweep(arm, self.record, second, self.now + timedelta(minutes=10))
        self.assertEqual(third["empty_sweeps"], 0)
        arm.resources.clear()
        fourth = sweep(arm, self.record, third, self.now + timedelta(hours=1))
        fifth = sweep(arm, self.record, fourth, self.now + timedelta(hours=1, minutes=5))
        self.assertEqual(fifth["status"], "cleaned")


if __name__ == "__main__":
    unittest.main()
