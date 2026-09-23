import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const version = process.argv[2];
if (!/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(version ?? "")) {
  throw new Error("Usage: node scripts/prepare-release.mjs <major.minor.patch>");
}
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: root, stdio: "inherit", ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed (${result.status})`);
  return result.stdout;
}

// Release assets must come from the reviewed Git index. Local scratch files
// never belong in a release, even when they are not covered by .gitignore.
const files = run("git", ["ls-files", "--cached", "-z"], {
  stdio: ["ignore", "pipe", "inherit"], encoding: "utf8",
}).split("\0").filter(Boolean);
if (files.length === 0) {
  throw new Error("Release preparation requires tracked source files; run from a Git checkout with the library sources tracked.");
}

const cmakeFile = join(root, "CMakeLists.txt");
const previousCmake = await readFile(cmakeFile, "utf8");
const versionPattern = /^(project\(simdurl VERSION )\d+\.\d+\.\d+(?=\s)/m;
if (!versionPattern.test(previousCmake)) {
  throw new Error("Cannot find the simdurl project version in CMakeLists.txt.");
}
const build = join(root, "build", "release");
const dist = join(root, "dist");
const stage = join(dist, ".stage");
const packageName = `simdurl-${version}`;
const stagedSource = join(stage, packageName);
try {
  await rm(dist, { recursive: true, force: true });
  await mkdir(stagedSource, { recursive: true });
  for (const file of files) {
    const destination = join(stagedSource, file);
    await mkdir(dirname(destination), { recursive: true });
    await cp(join(root, file), destination);
  }
  // Stamp and test the packaged copy; releasing never edits or commits sources.
  await writeFile(join(stagedSource, "CMakeLists.txt"),
    previousCmake.replace(versionPattern, (_, prefix) => `${prefix}${version}`));
  await rm(build, { recursive: true, force: true });
  run("cmake", ["-S", stagedSource, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DSIMDURL_BUILD_TESTS=ON"]);
  run("cmake", ["--build", build, "--config", "Release", "--parallel"]);
  run("ctest", ["--test-dir", build, "-C", "Release", "--output-on-failure"]);

  const archive = join(dist, `${packageName}.tar.gz`);
  run("cmake", ["-E", "tar", "czf", archive, "--format=gnutar", packageName], { cwd: stage });
  const checksum = createHash("sha256").update(await readFile(archive)).digest("hex");
  await writeFile(join(dist, "SHA256SUMS"), `${checksum}  ${packageName}.tar.gz\n`);
  console.log(`Prepared ${archive}`);
} finally {
  await rm(stage, { recursive: true, force: true });
}
