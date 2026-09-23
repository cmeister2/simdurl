#!/usr/bin/env python3
"""Exercise commit selection against real, isolated Git histories."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import benchmark_commits


SCRIPT = Path(__file__).with_name("benchmark_commits.py")


class CommitRangeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repository = Path(self.temporary.name)
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Benchmark test")
        self.git("config", "user.email", "benchmark@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.base = self.commit("initial")

    def git(self, *arguments, input=None):
        return subprocess.run(
            ["git", "-C", str(self.repository), *arguments],
            input=input, capture_output=True, text=True, check=True,
        ).stdout.strip()

    def commit(self, message):
        self.git("commit", "--allow-empty", "--message", message)
        return self.git("rev-parse", "HEAD")

    def plan(self, before, after, main_head=None):
        return benchmark_commits.plan_commits(self.repository, before, after, main_head)

    def test_multi_commit_push_in_chronological_order(self):
        first = self.commit("first")
        second = self.commit("second")
        self.assertEqual(self.plan(self.base, second), [first, second])

    def test_merged_branch_commits_are_excluded(self):
        self.git("checkout", "-b", "feature")
        feature = self.commit("feature")
        self.git("checkout", "main")
        main = self.commit("main")
        self.git("merge", "--no-ff", "feature", "--message", "merge")
        merge = self.git("rev-parse", "HEAD")
        after = self.commit("after merge")
        self.assertEqual(self.plan(self.base, after), [main, merge, after])
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "first-parent"):
            self.plan(feature, after)

    def test_manual_endpoints_must_be_on_main_first_parent_history(self):
        self.git("checkout", "-b", "feature")
        feature = self.commit("feature")
        self.git("checkout", "main")
        main = self.commit("main")
        self.git("merge", "--no-ff", "feature", "--message", "merge")
        merge = self.git("rev-parse", "HEAD")
        self.assertEqual(self.plan("", main, main_head=merge), [main])
        self.assertEqual(self.plan(self.base, merge, main_head=merge), [main, merge])
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "--main-head"):
            self.plan("", feature, main_head=merge)

        self.git("checkout", "-b", "unmerged")
        unmerged = self.commit("unmerged")
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "--main-head"):
            self.plan("", unmerged, main_head=merge)
        self.git("checkout", "--orphan", "unrelated")
        unrelated = self.commit("unrelated")
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "--main-head"):
            self.plan("", unrelated, main_head=merge)

    def test_main_head_must_exist(self):
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "unavailable"):
            self.plan("", self.base, main_head="f" * 40)

    def test_unrelated_and_reversed_ranges_are_rejected(self):
        after = self.commit("after")
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "ancestor"):
            self.plan(after, self.base)
        self.git("checkout", "--orphan", "unrelated")
        unrelated = self.commit("unrelated")
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "ancestor"):
            self.plan(unrelated, after)

    def test_single_commit_and_empty_ranges(self):
        after = self.commit("after")
        self.assertEqual(self.plan(None, after), [after])
        self.assertEqual(self.plan("", after), [after])
        self.assertEqual(self.plan("0" * 40, after), [after])
        self.assertEqual(self.plan(after, after), [])

    def test_missing_commits_are_rejected(self):
        for before, after in [("f" * 40, self.base), ("", "f" * 40)]:
            with self.subTest(before=before, after=after):
                with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "unavailable"):
                    self.plan(before, after)

    def test_refs_and_shell_text_are_rejected(self):
        for value in ["HEAD", "main", self.base[:12], "--all", "HEAD^{commit}",
                      "$(touch should-not-exist)", "a" * 40 + "\n"]:
            with self.subTest(value=value):
                with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "40-character"):
                    self.plan(value, self.base)
                with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "40-character"):
                    self.plan("", value)
                with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "40-character"):
                    self.plan("", self.base, main_head=value)
        self.assertFalse((self.repository / "should-not-exist").exists())

    def test_matrix_limit(self):
        tree = self.git("rev-parse", "HEAD^{tree}")
        parent = self.base
        commits = []
        # commit-tree constructs a long real history without checking out every commit.
        for index in range(257):
            parent = self.git("commit-tree", tree, "-p", parent, input=f"commit {index}\n")
            commits.append(parent)
        self.assertEqual(self.plan(self.base, commits[255]), commits[:256])
        with self.assertRaisesRegex(benchmark_commits.CommitRangeError, "Split the range"):
            self.plan(self.base, commits[256])

    def test_cli_emits_compact_matrix_and_errors_on_stderr(self):
        for before, expected in [("", [self.base]), (self.base, [])]:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--repository", str(self.repository),
                 "--before", before, "--after", self.base, "--main-head", self.base],
                capture_output=True, text=True, check=True,
            )
            self.assertEqual(result.stdout, json.dumps({"commit": expected}, separators=(",", ":")) + "\n")
            self.assertEqual(result.stderr, "")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--repository", str(self.repository),
             "--after", "main"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("40-character", result.stderr)


if __name__ == "__main__":
    unittest.main()
