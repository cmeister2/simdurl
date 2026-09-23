"""Cloud-independent ownership contract shared by the controller and reaper."""

from datetime import datetime, timedelta, timezone
import re

RUN_ID = re.compile(r"simdurl-[a-z0-9]+(?:-[a-z0-9]+)*\Z")
SUBSCRIPTION_ID = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")
RESOURCE_GROUP = re.compile(r"[A-Za-z0-9_.()-]{1,90}\Z")
OWNER_TAG = "simdurl-owner"
RUN_TAG = "simdurl-run"


def parse_time(value):
    """Require an explicit timezone; normalize timestamps to UTC."""
    if not isinstance(value, str):
        raise ValueError("timestamp must be a string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError("invalid timestamp") from exc
    if parsed.tzinfo is None:
        raise ValueError("timestamp must include a timezone")
    return parsed.astimezone(timezone.utc)


def expected_resource_ids(run_id, subscription_id, resource_group):
    if not isinstance(run_id, str) or len(run_id) > 40 or not RUN_ID.fullmatch(run_id):
        raise ValueError("run_id must be simdurl- followed by lowercase alphanumeric groups, at most 40 characters")
    if not isinstance(subscription_id, str) or not SUBSCRIPTION_ID.fullmatch(subscription_id):
        raise ValueError("invalid subscription_id")
    if not isinstance(resource_group, str) or not RESOURCE_GROUP.fullmatch(resource_group):
        raise ValueError("invalid resource_group")
    prefix = f"/subscriptions/{subscription_id}/resourceGroups/{resource_group}/providers"
    return {
        "vm": f"{prefix}/Microsoft.Compute/virtualMachines/{run_id}-vm",
        "disk": f"{prefix}/Microsoft.Compute/disks/{run_id}-disk",
        "nic": f"{prefix}/Microsoft.Network/networkInterfaces/{run_id}-nic",
        "public_ip": f"{prefix}/Microsoft.Network/publicIPAddresses/{run_id}-ip",
    }


def validate_manifest(manifest, subscription_id, resource_group):
    """Reject arbitrary resource IDs, groups and deployment names before cleanup."""
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise ValueError("unsupported manifest schema")
    if str(manifest.get("subscription_id", "")).lower() != subscription_id.lower():
        raise ValueError("manifest subscription differs from configured subscription")
    if str(manifest.get("resource_group", "")).lower() != resource_group.lower():
        raise ValueError("manifest resource group differs from configured compute group")
    expected = expected_resource_ids(manifest.get("run_id"), subscription_id, resource_group)
    actual = manifest.get("resource_ids")
    if not isinstance(actual, dict) or set(actual) != set(expected):
        raise ValueError("manifest must contain exactly the four expected resource IDs")
    if any(not isinstance(actual[k], str) or actual[k].lower() != v.lower() for k, v in expected.items()):
        raise ValueError("manifest resource IDs differ from deterministic run resources")
    if manifest.get("deployment_name") != manifest["run_id"]:
        raise ValueError("deployment_name must equal run_id")
    created, expires = parse_time(manifest.get("created_at")), parse_time(manifest.get("expires_at"))
    if not timedelta(0) < expires - created <= timedelta(hours=2):
        raise ValueError("allocation lifetime must be positive and at most two hours")


def resource_is_owned(kind, resource, manifest):
    """Permit only exact, registered IDs with matching ownership evidence.

    Azure creates the OS disk implicitly, before tags can be applied. Its exact
    pre-recorded ID, creation time and attachment provide the fallback evidence.
    """
    expected = manifest["resource_ids"].get(kind)
    if not expected or resource.get("id", "").lower() != expected.lower():
        return False
    tags = resource.get("tags") or {}
    if tags.get(OWNER_TAG) == "simdurl" and tags.get(RUN_TAG) == manifest["run_id"]:
        return True
    if kind != "disk" or OWNER_TAG in tags or RUN_TAG in tags:
        return False
    attached_to = resource.get("managedBy") or ""
    if attached_to and attached_to.lower() != manifest["resource_ids"]["vm"].lower():
        return False
    try:
        created = parse_time(resource.get("properties", {}).get("timeCreated"))
    except ValueError:
        return False
    return (parse_time(manifest["created_at"]) - timedelta(minutes=5)
            <= created <= parse_time(manifest["expires_at"]) + timedelta(hours=1))
