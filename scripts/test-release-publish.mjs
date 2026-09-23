import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { access, appendFile, chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { devNull, tmpdir } from "node:os";
import { join } from "node:path";
import { Writable } from "node:stream";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import semanticRelease from "semantic-release";
import config from "../release.config.cjs";

const require = createRequire(import.meta.url);
const root = fileURLToPath(new URL("..", import.meta.url));
const env = {
  PATH: process.env.PATH,
  HOME: process.env.HOME,
  GIT_CONFIG_GLOBAL: devNull,
  GIT_CONFIG_NOSYSTEM: "1",
  GIT_ALLOW_PROTOCOL: "file",
};
const git = (cwd, ...args) => execFileSync("git", [
  "-c", "user.name=Release Publish Test",
  "-c", "user.email=publish@example.invalid",
  "-c", "commit.gpgSign=false",
  ...args,
], { cwd, env, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();

test("publishing tags the tested commit without updating a protected release branch", async (t) => {
  const temporary = await mkdtemp(join(tmpdir(), "simdurl-publish-test-"));
  t.after(() => rm(temporary, { recursive: true, force: true }));
  const cwd = join(temporary, "checkout");
  const remote = join(temporary, "remote.git");
  await mkdir(join(cwd, "scripts"), { recursive: true });
  await copyFile(join(root, "scripts/prepare-release.mjs"), join(cwd, "scripts/prepare-release.mjs"));
  const cmake = [
    "cmake_minimum_required(VERSION 3.20)",
    "project(simdurl VERSION 0.0.0 LANGUAGES NONE)",
    "include(CMakePackageConfigHelpers)",
    'write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/simdurlConfigVersion.cmake"',
    '  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)',
    "enable_testing()",
    'add_test(NAME release_fixture COMMAND "${CMAKE_COMMAND}" -E true)',
    "",
  ].join("\n");
  await writeFile(join(cwd, "CMakeLists.txt"), cmake);
  await writeFile(join(cwd, ".gitignore"), "build/\ndist/\n");
  await writeFile(join(cwd, "README.md"), "Release fixture\n");
  git(cwd, "init", "--quiet", "--initial-branch=main");
  git(cwd, "add", ".");
  git(cwd, "commit", "--quiet", "-m", "chore: initial repository");
  git(cwd, "tag", "0.0.0");
  await appendFile(join(cwd, "README.md"), "Encoding is available\n");
  git(cwd, "commit", "--quiet", "-am", "feat: add URL encoding");
  git(temporary, "clone", "--quiet", "--bare", cwd, remote);
  git(cwd, "remote", "add", "origin", pathToFileURL(remote).href);
  git(cwd, "fetch", "--quiet", "origin");
  git(cwd, "branch", "--set-upstream-to=origin/main", "main");

  // Tags and release notes are allowed; any branch update must reject the push.
  const hook = join(remote, "hooks", "pre-receive");
  await writeFile(hook, `#!/bin/sh
while read old new ref; do
  printf '%s\\n' "$ref" >> received-refs
  case "$ref" in
    refs/heads/*) echo "Required Success check blocks branch updates" >&2; exit 1 ;;
  esac
done
`);
  await chmod(hook, 0o755);
  assert.throws(() => git(cwd, "push", "origin", "HEAD:refs/heads/blocked-branch"),
    /Required Success check blocks branch updates/);
  await writeFile(join(remote, "received-refs"), "");
  await writeFile(join(cwd, "scratch.txt"), "Keep local scratch files out of the archive\n");
  const head = git(cwd, "rev-parse", "HEAD");
  const status = git(cwd, "status", "--porcelain=v1", "--untracked-files=all");
  let published = false;
  const plugins = config.plugins.map(([name, options]) => name === "@semantic-release/github"
    ? [{ publish: async (_options, context) => {
      assert.equal(context.nextRelease.version, "0.1.0");
      const archiveName = "simdurl-0.1.0.tar.gz";
      const archive = await readFile(join(cwd, "dist", archiveName));
      assert.equal(await readFile(join(cwd, "dist/SHA256SUMS"), "utf8"),
        `${createHash("sha256").update(archive).digest("hex")}  ${archiveName}\n`);
      published = true;
      return { name: "Local release fixture" };
    } }, options]
    : [require.resolve(name), options]);
  const output = new Writable({ write(_chunk, _encoding, done) { done(); } });
  t.after(() => output.end());

  const result = await semanticRelease({
    ...config, repositoryUrl: pathToFileURL(remote).href, ci: false, dryRun: false, plugins,
  }, { cwd, env, stdout: output, stderr: output });

  assert.equal(published, true);
  assert.equal(result.nextRelease.version, "0.1.0");
  assert.equal(result.nextRelease.gitHead, head);
  assert.equal(git(remote, "rev-parse", "refs/tags/0.1.0"), head);
  assert.equal(git(remote, "rev-parse", "refs/heads/main"), head);
  assert.equal(git(cwd, "rev-parse", "HEAD"), head);
  assert.equal(git(cwd, "status", "--porcelain=v1", "--untracked-files=all"), status);
  assert.equal(await readFile(join(cwd, "CMakeLists.txt"), "utf8"), cmake);
  const received = (await readFile(join(remote, "received-refs"), "utf8")).trim().split("\n");
  assert.ok(received.includes("refs/tags/0.1.0"));
  assert.ok(received.some((ref) => ref.startsWith("refs/notes/")));
  assert.ok(received.every((ref) => /^refs\/(tags|notes)\//.test(ref)), received.join("\n"));

  const unpacked = join(temporary, "unpacked");
  await mkdir(unpacked);
  execFileSync("cmake", ["-E", "tar", "xzf", join(cwd, "dist/simdurl-0.1.0.tar.gz")], { cwd: unpacked, env });
  assert.equal(await readFile(join(unpacked, "simdurl-0.1.0/CMakeLists.txt"), "utf8"),
    cmake.replace("VERSION 0.0.0", "VERSION 0.1.0"));
  assert.match(await readFile(join(cwd, "build/release/simdurlConfigVersion.cmake"), "utf8"),
    /set\(PACKAGE_VERSION "0\.1\.0"\)/);
  await assert.rejects(access(join(unpacked, "simdurl-0.1.0/scratch.txt")), { code: "ENOENT" });
});
