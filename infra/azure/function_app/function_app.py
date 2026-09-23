"""Independent five-minute cleanup; never allocates resources.

The package builder includes ../lifecycle.py at this package's root. Every HTTP
operation is bounded. A pending deletion is resumed on the next timer tick.
"""

from datetime import datetime, timezone
import json
import logging
import os
import time

import azure.functions as func
from azure.core.exceptions import ResourceNotFoundError
from azure.identity import ManagedIdentityCredential
from azure.storage.blob import BlobServiceClient, ContentSettings
import requests

from lifecycle import parse_time, validate_manifest
from reaper import sweep

app = func.FunctionApp()
API_VERSIONS = {
    "virtualMachines": "2024-07-01",
    "disks": "2024-03-02",
    "networkInterfaces": "2024-05-01",
    "publicIPAddresses": "2024-05-01",
    "deployments": "2022-09-01",
}


def utc_now():
    return datetime.now(timezone.utc)


def timestamp(value):
    return value.isoformat().replace("+00:00", "Z")


class ArmClient:
    def __init__(self, credential):
        self.credential = credential
        self.session = requests.Session()
        self.deadline = time.monotonic() + 180

    def request(self, method, resource_id, action=""):
        if time.monotonic() > self.deadline - 30:
            raise RuntimeError("reaper ARM request budget exhausted")
        resource_type = resource_id.split("/")[-2]
        version = API_VERSIONS[resource_type]
        token = self.credential.get_token("https://management.azure.com/.default").token
        response = self.session.request(
            method,
            f"https://management.azure.com{resource_id}{action}?api-version={version}",
            headers={"Authorization": f"Bearer {token}"},
            timeout=(10, 20),
        )
        if response.status_code == 404:
            return None
        if not response.ok:
            # Do not log tokens, protected command parameters or response bodies.
            raise RuntimeError(f"ARM {method} {resource_id}{action}: HTTP {response.status_code}")
        return response.json() if response.content else {}

    def get(self, resource_id):
        return self.request("GET", resource_id)

    def delete(self, resource_id):
        return self.request("DELETE", resource_id)


def read_json(container, name):
    try:
        return json.loads(container.download_blob(name, timeout=20).readall())
    except ResourceNotFoundError:
        return None


def write_json(container, name, data):
    container.upload_blob(
        name,
        json.dumps(data, sort_keys=True),
        overwrite=True,
        content_settings=ContentSettings(content_type="application/json"),
        timeout=20,
    )


@app.timer_trigger(schedule="0 */5 * * * *", arg_name="timer", run_on_startup=False, use_monitor=True)
def reap_expired_runs(timer: func.TimerRequest):
    credential = ManagedIdentityCredential(client_id=os.environ["AZURE_CLIENT_ID"])
    service = BlobServiceClient(
        os.environ["CONTROL_ACCOUNT_URL"], credential=credential,
        connection_timeout=10, read_timeout=20, retry_total=1,
    )
    manifests = service.get_container_client("runs")
    states = service.get_container_client("state")
    arm = ArmClient(credential)
    started = time.monotonic()
    heartbeat = {"schema_version": 1, "updated_at": timestamp(utc_now()), "status": "ok", "checked_runs": 0, "pending_runs": 0, "errors": []}
    try:
        # Avoid two storage reads per historical run on every timer invocation.
        # Run manifests and individual tombstones remain available for auditing.
        tombstones = read_json(states, "reaper-tombstones.json") or {"run_ids": []}
        cleaned = set(tombstones["run_ids"])
        new_tombstones = False
        for blob in manifests.list_blobs(timeout=20):
            if not blob.name.endswith(".json"):
                continue
            if blob.name[:-5] in cleaned:
                continue
            if time.monotonic() - started > 180:
                raise RuntimeError("reaper sweep exceeded its time budget")
            try:
                manifest = read_json(manifests, blob.name)
                validate_manifest(manifest, os.environ["COMPUTE_SUBSCRIPTION_ID"], os.environ["COMPUTE_RESOURCE_GROUP"])
                if blob.name != f"{manifest['run_id']}.json":
                    raise ValueError("manifest blob name differs from run_id")
                state_name = f"reaper/{manifest['run_id']}.json"
                previous = read_json(states, state_name) or {}
                heartbeat["checked_runs"] += 1
                if previous.get("status") == "cleaned":
                    cleaned.add(manifest["run_id"])
                    new_tombstones = True
                    continue
                now = utc_now()
                if (parse_time(manifest["expires_at"]) > now
                        and manifest.get("status") not in {"cleanup_pending", "complete", "completed", "failed", "cleaned"}
                        and previous.get("status") != "cleanup_pending"):
                    continue
                state = sweep(arm, manifest, previous, now)
                write_json(states, state_name, state)
                if state["status"] != "cleaned":
                    heartbeat["pending_runs"] += 1
                else:
                    cleaned.add(manifest["run_id"])
                    new_tombstones = True
                if state["errors"]:
                    raise RuntimeError(f"cleanup errors for {manifest['run_id']}: " + "; ".join(state["errors"]))
            except Exception as exc:
                heartbeat["errors"].append(str(exc))
                logging.error("simdurl-reaper-error %s", exc)
        if new_tombstones:
            write_json(states, "reaper-tombstones.json", {"schema_version": 1, "run_ids": sorted(cleaned)})
    except Exception as exc:
        heartbeat["errors"].append(str(exc))
        logging.error("simdurl-reaper-error %s", exc)
    heartbeat["updated_at"] = timestamp(utc_now())
    if heartbeat["errors"]:
        heartbeat["status"] = "error"
    write_json(states, "heartbeat.json", heartbeat)
    if heartbeat["status"] == "ok":
        logging.info("simdurl-reaper-heartbeat")
