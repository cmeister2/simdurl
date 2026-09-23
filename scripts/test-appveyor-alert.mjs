import assert from 'node:assert/strict';
import { test } from 'node:test';
import updateAlert from './appveyor-alert.mjs';

function fixture() {
  const build = {
    buildId: 123, version: '34', branch: 'main', commitId: 'abc123',
    status: 'failed', finished: '2026-09-23T03:30:00Z'
  };
  const context = {
    eventName: 'status',
    repo: { owner: 'cmeister2', repo: 'simdurl' },
    payload: {
      context: 'continuous-integration/appveyor/branch', state: 'failure',
      target_url: 'https://ci.appveyor.com/project/cmeister2/simdurl/builds/123',
      sha: 'abc123', repository: { default_branch: 'main' }, branches: []
    }
  };
  const issues = [];
  const writes = [];
  const comments = [];
  const api = {
    listForRepo() {},
    async create(input) {
      writes.push(input);
      issues.push({ ...input, number: 7, state: 'open', user: { login: 'github-actions[bot]' } });
    },
    async update(input) {
      writes.push(input);
      Object.assign(issues.find(issue => issue.number === input.issue_number), input);
    },
    async createComment(input) {
      writes.push(input);
      comments.push(input.body);
    }
  };
  const github = {
    rest: { issues: api },
    async paginate(method, input) {
      assert.equal(method, api.listForRepo);
      assert.equal(input.state, 'all');
      assert.equal(input.creator, 'github-actions[bot]');
      return structuredClone(issues);
    }
  };
  const input = {
    context, github, core: { notice() {} },
    async fetch(url) {
      assert.equal(url, 'https://ci.appveyor.com/api/projects/cmeister2/simdurl/branch/main');
      return { ok: true, json: async () => ({ build }) };
    }
  };
  return { build, context, issues, writes, comments, input };
}

test('opens one alert, updates failures quietly, closes on recovery, and reopens on recurrence', async () => {
  const { build, context, issues, comments, input } = fixture();
  await updateAlert(input);
  assert.equal(issues.length, 1);
  assert.equal(issues[0].state, 'open');
  assert.deepEqual(issues[0].assignees, ['cmeister2']);
  assert.match(issues[0].body, /builds\/123/);
  assert.match(issues[0].body, /abc123/);

  build.buildId = 124;
  context.payload.target_url = context.payload.target_url.replace('/123', '/124');
  await updateAlert(input);
  assert.equal(issues.length, 1);
  assert.match(issues[0].body, /builds\/124/);
  assert.equal(comments.length, 0);

  build.status = 'success';
  context.payload.state = 'success';
  await updateAlert(input);
  assert.equal(issues[0].state, 'closed');
  assert.equal(issues[0].state_reason, 'completed');
  assert.match(comments[0], /Recovered/);
  await updateAlert(input);
  assert.equal(comments.length, 1);

  build.status = 'failed';
  context.payload.state = 'error';
  await updateAlert(input);
  assert.equal(issues.length, 1);
  assert.equal(issues[0].state, 'open');
  assert.match(comments[1], /@cmeister2.*failing again/);
});

test('a healthy build does not create an issue', async () => {
  const { build, input, writes } = fixture();
  build.status = 'success';
  await updateAlert(input);
  assert.deepEqual(writes, []);
});

test('ignores unrelated events, stale builds, PRs, and inconclusive results', async () => {
  const changes = [
    ({ context }) => { context.eventName = 'push'; },
    ({ context }) => { context.payload.context = 'other-ci'; },
    ({ context }) => { context.payload.state = 'pending'; },
    ({ context }) => { context.payload.target_url = 'https://example.com/'; },
    ({ context }) => { context.payload.target_url = null; },
    ({ context }) => { context.payload.sha = 'older-commit'; },
    ({ build }) => { build.buildId = 999; },
    ({ build }) => { build.branch = 'feature'; },
    ({ build }) => { build.pullRequestId = 42; },
    ({ build }) => { build.status = 'running'; },
    ({ build }) => { build.status = 'cancelled'; }
  ];
  for (const change of changes) {
    const current = fixture();
    change(current);
    await updateAlert(current.input);
    assert.deepEqual(current.writes, []);
  }
});

test('only manages bot-created alert issues, not user issues or pull requests', async () => {
  const { issues, writes, input } = fixture();
  const body = '<!-- simdurl-appveyor-alert:v1 -->';
  issues.push(
    { number: 1, body, user: { login: 'someone' } },
    { number: 2, body, user: { login: 'github-actions[bot]' }, pull_request: {} },
    { number: 3, body: 'Another bot issue', user: { login: 'github-actions[bot]' } }
  );
  await updateAlert(input);
  assert.equal(writes.length, 1);
  assert.equal(writes[0].issue_number, undefined);
  assert.equal(issues.length, 4);
});

test('a failed AppVeyor lookup fails the job without changing issues', async () => {
  const { input, writes } = fixture();
  input.fetch = async () => ({ ok: false, status: 503 });
  await assert.rejects(updateAlert(input), /HTTP 503/);
  assert.deepEqual(writes, []);
});