#!/usr/bin/env python3
"""Emit metrics on stdout and preserve measurement evidence in Bencher job stderr."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="simdurl-benchmark-") as directory:
    output = Path(directory)
    command = [
        sys.executable, str(root / "benchmark.py"),
        "--bin-dir", str(root / "benchmarks"),
        "--output-dir", str(output),
        "--commit", (root / "benchmarks/source-sha").read_text().strip(),
    ]
    # Extra arguments make it possible to smoke-test the identical image locally.
    command.extend(sys.argv[1:])
    backend = json.loads(subprocess.check_output([root / "benchmarks/backend_info"], text=True))
    result = subprocess.run(command, check=False)
    evidence = {
        "dispatch": backend,
        "compiler_flags": (root / "benchmarks/compiler-flags").read_text().strip(),
        "files": {
            str(path.relative_to(output)): path.read_text()
            for path in sorted(output.rglob("*")) if path.is_file()
        },
    }
    # The remote filesystem is ephemeral. Job output is retained by Bencher and
    # downloaded by CI, so carry raw samples, stdout/stderr and metadata with it.
    print(json.dumps({"simdurl_evidence": evidence}, separators=(",", ":")), file=sys.stderr)
    sys.exit(result.returncode)
