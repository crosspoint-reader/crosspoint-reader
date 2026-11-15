const {comparison} = require('./firmware-size-comparison');
module.exports = async ({github, context, core}) => {
  const marker = '<!-- pr-firmware-artifacts -->';
  const run = context.payload.workflow_run;
  const { owner, repo } = context.repo;

  const headOwner = run.head_repository?.owner?.login;
  if (!headOwner) {
    core.notice(`No head repository found for ${run.head_sha}.`);
    return;
  }

  const pullRequests = await github.paginate(github.rest.pulls.list, {
    owner,
    repo,
    head: `${headOwner}:${run.head_branch}`,
    state: 'all',
    per_page: 100,
  });
  const associatedPullRequest = pullRequests.find(
    (pr) => pr.state === 'open' && pr.head.sha === run.head_sha,
  );

  if (!associatedPullRequest) {
    core.notice(`No pull request found for ${run.head_sha}.`);
    return;
  }

  const { data: pullRequest } = await github.rest.pulls.get({
    owner,
    repo,
    pull_number: associatedPullRequest.number,
  });

  if (pullRequest.state !== 'open' || pullRequest.head.sha !== run.head_sha) {
    core.notice(`Skipping closed or superseded workflow run for ${run.head_sha}.`);
    return;
  }

  const builds = [
    { env: 'default', name: 'firmware.bin', device: 'X3 / X4' },
    { env: 'sticky', name: 'firmware-sticky.bin', device: 'Sticky' },
    { env: 'x4pro', name: 'firmware-x4pro.bin', device: 'X4 Pro' },
    { env: 'x4c', name: 'firmware-x4c.bin', device: 'X4 Classic' },
    { env: 'papermono', name: 'firmware-papermono.bin', device: 'Paper Mono' },
    { env: 'metalio_eink4', name: 'firmware-metalio_eink4.bin', device: 'MetalIO EInk4' },
    { env: 'eego_a4', name: 'firmware-eego_a4.bin', device: 'EEGO A4' },
    { env: 'picco', name: 'firmware-picco.bin', device: 'BOOX Picco' },
  ];
  const artifactResponse =
    await github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
      owner,
      repo,
      run_id: run.id,
      per_page: 100,
    });
  const artifacts = new Map(
    artifactResponse
      .filter((artifact) => !artifact.expired)
      .map((artifact) => [artifact.name, artifact]),
  );
  const missingBuilds = builds.filter((build) => !artifacts.has(build.name));
  const shortSha = run.head_sha.slice(0, 7);
  let body = `${marker}\n### Firmware builds\n\n`;

  body += `Automatically generated for the latest commit of this PR (\`${shortSha}\`).\n\n`;
  const links = builds.filter(build => artifacts.has(build.name)).map(({name, device}) => {
    const artifact = artifacts.get(name);
    return `- [${device} (\`${name}\`)](https://github.com/${owner}/${repo}/actions/runs/${run.id}/artifacts/${artifact.id})`;
  });
  if (links.length) {
    body += links.join('\n') + '\n\nDownload and extract the `.bin` for your device, then flash it with ';
    body += '[CrossPoint Reader Flash Tools](https://crosspointreader.com/#flash-tools). ';
    body += '**Make sure you use the correct firmware file for your device.**';
  }
  if (missingBuilds.length) body += `\n\nFirmware unavailable: ${missingBuilds.map(b => b.device).join(', ')}.`;
  if (run.conclusion !== 'success') body += `\n\n[CI finished with ${run.conclusion}](${run.html_url}).`;
  body += await comparison({github, context, core, run, pullRequest, builds});

  const comments = await github.paginate(github.rest.issues.listComments, {
    owner,
    repo,
    issue_number: pullRequest.number,
    per_page: 100,
  });
  const existingComment = comments.find(
    (comment) =>
      comment.user?.login === 'github-actions[bot]' && comment.body?.includes(marker),
  );

  // A delayed completion or rerun must not replace a newer run for this head.
  const latestRuns = await github.paginate(github.rest.actions.listWorkflowRuns, {
    owner, repo, workflow_id: run.workflow_id, event: 'pull_request',
    head_sha: run.head_sha, per_page: 100,
  });
  if (latestRuns.some(candidate => candidate.head_repository?.id === run.head_repository.id &&
      candidate.head_branch === run.head_branch &&
      (candidate.id > run.id || (candidate.id === run.id && candidate.run_attempt > run.run_attempt)))) {
    core.notice('Skipping superseded run or attempt.');
    return;
  }
  const {data: currentPull} = await github.rest.pulls.get({owner, repo, pull_number: pullRequest.number});
  if (currentPull.state !== 'open' || currentPull.head.sha !== run.head_sha ||
      currentPull.base.ref !== pullRequest.base.ref) return;

  if (existingComment) {
    await github.rest.issues.updateComment({
      owner,
      repo,
      comment_id: existingComment.id,
      body,
    });
  } else {
    await github.rest.issues.createComment({
      owner,
      repo,
      issue_number: pullRequest.number,
      body,
    });
  }
};
