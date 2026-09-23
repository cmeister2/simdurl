"""Pure, dependency-ordered cleanup logic used by the Azure timer."""

from datetime import timedelta

from lifecycle import parse_time, resource_is_owned

TERMINAL = {"Succeeded", "Failed", "Canceled"}


def sweep(arm, manifest, previous, now):
    """One dependency-ordered attempt, with no unbounded polling."""
    state = {
        "schema_version": 1,
        "run_id": manifest["run_id"],
        "checked_at": now.isoformat().replace("+00:00", "Z"),
        "cleanup_started_at": previous.get("cleanup_started_at", now.isoformat().replace("+00:00", "Z")),
        "status": "cleanup_pending",
        "remaining": [],
        "errors": [],
        "deployment_terminal": False,
        "empty_sweeps": 0,
    }
    subscription, group = manifest["subscription_id"], manifest["resource_group"]
    deployment_id = f"/subscriptions/{subscription}/resourceGroups/{group}/providers/Microsoft.Resources/deployments/{manifest['deployment_name']}"
    try:
        deployment = arm.get(deployment_id)
        state["deployment_terminal"] = deployment is None or deployment.get("properties", {}).get("provisioningState") in TERMINAL
        if not state["deployment_terminal"]:
            arm.request("POST", deployment_id, "/cancel")
    except Exception as exc:
        state["errors"].append(str(exc))

    # Do not allow a failed ownership check on a VM to authorize its companions.
    resources = {}
    ownership_errors = False
    for kind, resource_id in manifest["resource_ids"].items():
        try:
            resource = arm.get(resource_id)
            if resource is not None and not resource_is_owned(kind, resource, manifest):
                raise ValueError(f"ownership mismatch: {kind}")
            resources[kind] = resource
        except Exception as exc:
            state["errors"].append(str(exc))
            ownership_errors = True
    if ownership_errors:
        state["remaining"] = list(manifest["resource_ids"].values())
        return state

    vm_id = manifest["resource_ids"]["vm"]
    if resources["vm"] is not None:
        delete_failed = False
        try:
            arm.delete(vm_id)
        except Exception as exc:
            state["errors"].append(str(exc))
            delete_failed = True
        if delete_failed or now - parse_time(state["cleanup_started_at"]) >= timedelta(minutes=10):
            try:
                arm.request("POST", vm_id, "/deallocate")
            except Exception as stop_exc:
                state["errors"].append(str(stop_exc))
        # An accepted DELETE is not proof that the VM has disappeared.
        state["remaining"] = [resource_id for kind, resource_id in manifest["resource_ids"].items() if resources[kind] is not None]
        return state

    for kind in ("disk", "nic", "public_ip"):
        if resources[kind] is None:
            continue
        if kind == "public_ip" and resources["nic"] is not None:
            state["remaining"].append(manifest["resource_ids"][kind])
            continue
        try:
            arm.delete(manifest["resource_ids"][kind])
        except Exception as exc:
            state["errors"].append(str(exc))
        state["remaining"].append(manifest["resource_ids"][kind])

    if not state["remaining"] and not state["errors"] and state["deployment_terminal"]:
        state["empty_sweeps"] = int(previous.get("empty_sweeps", 0)) + 1
        # Recheck tombstones after the expiry horizon, allowing late creations
        # from a cancelled ARM deployment to be caught independently of CI.
        if state["empty_sweeps"] >= 2 and now >= parse_time(manifest["expires_at"]) + timedelta(hours=1):
            state["status"] = "cleaned"
    return state

