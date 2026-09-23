export default async function updateAlert({ github, context, core, fetch = globalThis.fetch }) {
  const { owner, repo } = context.repo;
  const event = context.payload;
  const buildUrlPrefix = `https://ci.appveyor.com/project/${owner}/${repo}/builds/`;
  if (context.eventName !== 'status' ||
      event.context !== 'continuous-integration/appveyor/branch' ||
      !['success', 'failure', 'error'].includes(event.state) ||
      !event.target_url?.startsWith(buildUrlPrefix)) {
    return;
  }

  const branch = event.repository.default_branch;
  const response = await fetch(
    `https://ci.appveyor.com/api/projects/${owner}/${repo}/branch/${encodeURIComponent(branch)}`,
    { signal: AbortSignal.timeout(30_000) }
  );
  if (!response.ok) {
    throw new Error(`AppVeyor build lookup failed: HTTP ${response.status}`);
  }
  const { build } = await response.json();
  if (build.branch !== branch || build.pullRequestId ||
      event.target_url !== `${buildUrlPrefix}${build.buildId}` ||
      event.sha !== build.commitId || !['success', 'failed'].includes(build.status)) {
    core.notice('Ignoring a superseded, non-default-branch, or incomplete AppVeyor build');
    return;
  }

  await updateIssue({ github, context }, {
    failed: build.status === 'failed',
    runLink: `[AppVeyor build ${build.version}](${event.target_url})`,
    details: [
      `Branch: \`${build.branch}\``,
      `Commit: \`${build.commitId}\``,
      `Finished: ${build.finished}`
    ]
  });
}

async function updateIssue({ github, context }, { failed, runLink, details }) {
  const { owner, repo } = context.repo;
  const title = '[automation] AppVeyor is failing';
  const marker = '<!-- simdurl-appveyor-alert:v1 -->';
  const issues = await github.paginate(github.rest.issues.listForRepo, {
    owner, repo, state: 'all', creator: 'github-actions[bot]', per_page: 100
  });
  const alert = issues
    .filter(issue => !issue.pull_request &&
      issue.user?.login === 'github-actions[bot]' && issue.body?.includes(marker))
    .sort((first, second) => first.number - second.number)[0];

  if (failed) {
    const body = [
      marker,
      `@${owner}, the scheduled AppVeyor Windows checks are failing.`,
      '',
      `Latest failure: ${runLink}`,
      ...details,
      '',
      'This issue is maintained automatically and closes after the next successful build.'
    ].join('\n');
    if (!alert) {
      await github.rest.issues.create({ owner, repo, title, body, assignees: [owner] });
    } else {
      await github.rest.issues.update({
        owner, repo, issue_number: alert.number, title, body, state: 'open'
      });
      if (alert.state === 'closed') {
        await github.rest.issues.createComment({
          owner, repo, issue_number: alert.number,
          body: `@${owner}, AppVeyor is failing again: ${runLink}.`
        });
      }
    }
    return;
  }

  if (alert?.state === 'open') {
    await github.rest.issues.createComment({
      owner, repo, issue_number: alert.number,
      body: `Recovered in ${runLink}.`
    });
    await github.rest.issues.update({
      owner, repo, issue_number: alert.number, state: 'closed', state_reason: 'completed'
    });
  }
}