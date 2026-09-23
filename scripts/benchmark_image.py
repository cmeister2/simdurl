#!/usr/bin/env python3
"""Verify and load benchmark image archives without executing their contents."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

from benchmark_azure_guest import GuestError, archive_image


def inspect_image(reference):
    return subprocess.check_output(
        ["docker", "image", "inspect", "--format", "{{.Id}}", reference],
        text=True, timeout=60,
    ).strip()


def verify(archive, reference, expected_id):
    identity = archive_image(archive, expected_id)
    if identity["reference"] != reference:
        raise GuestError("Archive tag does not match the requested benchmark image")
    return identity


def load(archive, reference, expected_id, evidence_dir):
    identity = verify(archive, reference, expected_id)
    subprocess.run(["docker", "load", "--input", str(archive)], check=True, timeout=300)
    loaded = inspect_image(reference)
    if loaded not in identity["image_ids"]:
        raise GuestError("Loaded Docker image is not proven by the archive")
    evidence_dir.mkdir(parents=True, exist_ok=True)
    (evidence_dir / "archive-image.json").write_text(json.dumps(identity, sort_keys=True) + "\n")
    # The controller records this store's image ID in the run manifest.
    (evidence_dir / "controller-image-id.txt").write_text(loaded + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("verify", "load"):
        subparser = commands.add_parser(command)
        subparser.add_argument("--archive", type=Path, required=True)
        subparser.add_argument("--image", required=True)
        subparser.add_argument("--image-id-file", type=Path, required=command == "load",
                               help="Expected builder ID; verify otherwise inspects the local image")
        if command == "load":
            subparser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        expected_id = (args.image_id_file.read_text().strip() if args.image_id_file
                       else inspect_image(args.image))
        if args.command == "load":
            load(args.archive, args.image, expected_id, args.evidence_dir)
        else:
            verify(args.archive, args.image, expected_id)
    except (GuestError, OSError, subprocess.SubprocessError) as error:
        print(f"Benchmark image {args.command} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
