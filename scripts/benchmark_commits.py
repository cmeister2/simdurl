#!/usr/bin/env python3
"""Select every first-parent commit in a push for a GitHub Actions matrix."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


MAX_COMMITS = 256
ZERO_SHA = "0" * 40


class CommitRangeError(Exception):
    """The requested commit range cannot be benchmarked safely."""


def full_sha(value, label):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise CommitRangeError(f"{label} must be a full 40-character commit SHA")
    return value.lower()


def git(repository, *arguments, allow_failure=False):
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        capture_output=True, text=True, check=False,
    )
    if result.returncode and not allow_failure:
        raise CommitRangeError(result.stderr.strip() or "Git command failed")
    return result


def verify_commit(repository, sha, label):
    result = git(
        repository, "rev-parse", "--verify", "--end-of-options", f"{sha}^{{commit}}",
        allow_failure=True,
    )
    if result.returncode or result.stdout.strip() != sha:
        raise CommitRangeError(
            f"{label} commit {sha} is unavailable; fetch the complete Git history"
        )


def plan_commits(repository, before, after, main_head=None):
    """Return chronological mainline commits, excluding before and including after."""
    after = full_sha(after, "--after")
    before = full_sha(before, "--before") if before else None
    main_head = full_sha(main_head, "--main-head") if main_head is not None else None
    if before == ZERO_SHA:
        before = None
    verify_commit(repository, after, "--after")
    if main_head is not None:
        verify_commit(repository, main_head, "--main-head")
        mainline = git(repository, "rev-list", "--first-parent", main_head).stdout.splitlines()
        if after not in mainline:
            raise CommitRangeError("--after must be on the first-parent history of --main-head")
    if before is None:
        return [after]
    verify_commit(repository, before, "--before")
    if before == after:
        return []

    ancestor = git(repository, "merge-base", "--is-ancestor", before, after,
                   allow_failure=True)
    if ancestor.returncode == 1:
        raise CommitRangeError("--before must be an ancestor of --after")
    if ancestor.returncode:
        raise CommitRangeError(ancestor.stderr.strip() or "Cannot verify commit ancestry")
    first_parents = git(repository, "rev-list", "--first-parent", after).stdout.splitlines()
    if before not in first_parents:
        raise CommitRangeError("--before must be on the first-parent history of --after")

    commits = git(
        repository, "rev-list", "--first-parent", "--reverse", f"{before}..{after}"
    ).stdout.splitlines()
    if len(commits) > MAX_COMMITS:
        raise CommitRangeError(
            f"Range contains {len(commits)} commits; GitHub Actions permits at most "
            f"{MAX_COMMITS} matrix entries. Split the range into smaller ranges."
        )
    return commits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", default="", help="Exclusive starting commit SHA")
    parser.add_argument("--after", required=True, help="Inclusive ending commit SHA")
    parser.add_argument("--main-head", help="Require the ending commit to be on this mainline")
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        commits = plan_commits(args.repository, args.before, args.after, args.main_head)
    except (CommitRangeError, OSError) as error:
        print(f"benchmark commits: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"commit": commits}, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
