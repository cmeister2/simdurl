#!/usr/bin/env python3
"""Build the deployable Function zip; does not contact Azure.

Deploy the resulting archive with Azure Functions Core Tools/OneDeploy and a
remote Python build, so Linux dependencies are installed for Python 3.12.
"""

import argparse
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in ("function_app.py", "reaper.py", "host.json", "requirements.txt"):
            archive.write(root / "function_app" / name, name)
        archive.write(root / "lifecycle.py", "lifecycle.py")
    print(args.output)


if __name__ == "__main__":
    main()
