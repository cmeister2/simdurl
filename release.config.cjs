module.exports = {
  branches: ["main"],
  tagFormat: "${version}",
  plugins: [
    ["@semantic-release/commit-analyzer", { preset: "conventionalcommits" }],
    ["@semantic-release/release-notes-generator", { preset: "conventionalcommits" }],
    ["@semantic-release/exec", {
      prepareCmd: "node scripts/prepare-release.mjs ${nextRelease.version}",
    }],
    ["@semantic-release/github", {
      assets: [
        { path: "dist/simdurl-*.tar.gz" },
        { path: "dist/SHA256SUMS" },
      ],
      successCommentCondition: false,
      failCommentCondition: false,
      failTitle: false,
      releasedLabels: false,
    }],
  ],
};
