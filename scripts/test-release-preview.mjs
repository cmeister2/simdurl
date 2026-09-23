import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { appendFile, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { devNull, tmpdir } from "node:os";
import { join } from "node:path";
import { Writable } from "node:stream";
import test from "node:test";
import { previewRelease } from "./preview-release.mjs";

const env = {
  PATH: process.env.PATH,
  HOME: process.env.HOME,
  GIT_CONFIG_GLOBAL: devNull,
  GIT_CONFIG_NOSYSTEM: "1",
  GIT_ALLOW_PROTOCOL: "file",
};
const git = (cwd, ...args) => execFileSync("git", [
  "-c", "user.name=Release Preview Test",
  "-c", "user.email=preview@example.invalid",
  "-c", "commit.gpgSign=false",
  "-c", `core.hooksPath=${devNull}`,
  ...args,
], { cwd, env, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();

async function fixture(t) {
  const cwd = await mkdtemp(join(tmpdir(), "simdurl-preview-test-"));
  t.after(() => rm(cwd, { recursive: true, force: true }));
  git(cwd, "init", "--quiet", "--initial-branch=main");
  await commit(cwd, "chore: initial repository");
  git(cwd, "tag", "0.0.0");
  return cwd;
}

async function commit(cwd, message) {
  await appendFile(join(cwd, "README.md"), `${message}\n`);
  git(cwd, "add", "README.md");
  git(cwd, "commit", "--quiet", "-m", message);
}

async function snapshot(cwd) {
  return {
    head: git(cwd, "rev-parse", "HEAD"),
    branch: git(cwd, "rev-parse", "--abbrev-ref", "HEAD"),
    refs: git(cwd, "for-each-ref", "--format=%(refname) %(objectname)"),
    status: git(cwd, "status", "--porcelain=v1", "--untracked-files=all"),
    readme: await readFile(join(cwd, "README.md"), "utf8"),
    scratch: await readFile(join(cwd, "scratch.txt"), "utf8"),
  };
}

async function preview(cwd, extraEnv = {}) {
  const output = new Writable({ write(_chunk, _encoding, done) { done(); } });
  try {
    return await previewRelease({ cwd, env: { ...env, ...extraEnv }, stdout: output, stderr: output });
  } finally {
    output.end();
  }
}

test("detached fork PR previews 0.1.0 without credentials or changing its checkout", async (t) => {
  const cwd = await fixture(t);
  await commit(cwd, "feat: add form encoding");
  git(cwd, "checkout", "--quiet", "--detach");
  await appendFile(join(cwd, "README.md"), "Uncommitted local edit\n");
  await writeFile(join(cwd, "scratch.txt"), "Untracked local file\n");
  const before = await snapshot(cwd);

  const result = await preview(cwd, {
    GITHUB_ACTIONS: "true",
    GITHUB_EVENT_NAME: "pull_request",
    GITHUB_REF: "refs/pull/42/merge",
    GITHUB_HEAD_REF: "contributor-feature",
    GITHUB_BASE_REF: "main",
    GITHUB_REPOSITORY: "cmeister2/simdurl",
    GITHUB_SERVER_URL: "https://github.com",
    GITHUB_RUN_ID: "42",
  });

  assert.equal(result.nextRelease.version, "0.1.0");
  assert.equal(result.nextRelease.gitTag, "0.1.0");
  assert.match(result.nextRelease.notes, /add form encoding/);
  assert.ok(result.nextRelease.notes.includes("https://github.com/cmeister2/simdurl/compare/0.0.0...0.1.0"));
  assert.deepEqual(await snapshot(cwd), before);
  assert.equal(git(cwd, "tag", "--list", "0.1.0"), "");
});

test("branch previews honor existing tags and accept changes that need no release", async (t) => {
  const cwd = await fixture(t);
  await commit(cwd, "feat: initial release");
  git(cwd, "tag", "0.1.0");
  git(cwd, "checkout", "--quiet", "-b", "maintenance/fix");
  await commit(cwd, "fix: preserve malformed escapes");
  const patchHead = git(cwd, "rev-parse", "HEAD");

  const result = await preview(cwd);
  assert.equal(result.lastRelease.version, "0.1.0");
  assert.equal(result.nextRelease.version, "0.1.1");
  assert.match(result.nextRelease.notes, /preserve malformed escapes/);
  assert.equal(git(cwd, "rev-parse", "HEAD"), patchHead);
  assert.equal(git(cwd, "tag", "--list", "0.1.1"), "");

  git(cwd, "tag", "0.1.1");
  await commit(cwd, "docs: describe the installation");
  const docsHead = git(cwd, "rev-parse", "HEAD");
  assert.equal(await preview(cwd), false);
  assert.equal(git(cwd, "rev-parse", "HEAD"), docsHead);
  assert.equal(git(cwd, "tag", "--list", "0.1.2"), "");
});
