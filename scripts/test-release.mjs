import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { analyzeCommits } from "@semantic-release/commit-analyzer";
import { generateNotes } from "@semantic-release/release-notes-generator";
import config from "../release.config.cjs";

const pluginOptions = (name) => config.plugins.find(([plugin]) => plugin === name)[1];
const context = {
  cwd: fileURLToPath(new URL("..", import.meta.url)),
  options: { repositoryUrl: "https://github.com/cmeister2/simdurl.git" },
  logger: { log() {} },
  lastRelease: { version: "0.0.0", gitTag: "0.0.0" },
  nextRelease: { version: "0.1.0", gitTag: "0.1.0" },
  commits: [
    { hash: "a".repeat(40), message: "feat: add form encoding" },
    { hash: "b".repeat(40), message: "fix: preserve malformed escapes" },
  ],
};

// Exercise the configured plugins without publishing or requiring credentials.
test("release plugins analyze commits and render notes together", async () => {
  assert.equal(await analyzeCommits(
    pluginOptions("@semantic-release/commit-analyzer"), context,
  ), "minor");

  const notes = await generateNotes(
    pluginOptions("@semantic-release/release-notes-generator"), context,
  );
  assert.match(notes, /0\.1\.0/);
  assert.match(notes, /add form encoding/);
  assert.match(notes, /preserve malformed escapes/);
  assert.match(notes, /compare\/0\.0\.0\.\.\.0\.1\.0/);
});
