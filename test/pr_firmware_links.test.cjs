const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');
const { test } = require('node:test');

// Execute the actual github-script body without credentials or network access.
const workflow = readFileSync(resolve(__dirname, '../.github/workflows/pr-firmware-links.yml'), 'utf8');
const scriptLines = workflow.split('          script: |\n')[1].trimEnd().split('\n');
assert.ok(scriptLines.every((line) => !line || line.startsWith('            ')));
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const updateComment = new AsyncFunction('github', 'context', 'core',
  scriptLines.map((line) => line.slice(12)).join('\n'));
const marker = '<!-- pr-firmware-artifacts -->';
const sha = 'abc1234deadbeef';
const firmwareNames = ['firmware.bin', 'firmware-sticky.bin', 'firmware-x4pro.bin',
  'firmware-x4c.bin', 'firmware-papermono.bin', 'firmware-metalio_eink4.bin', 'firmware-eego_a4.bin'];
const previewNames = ['crosspoint-x4.html', 'crosspoint-x3.html', 'crosspoint-x4pro.html'];

function makeRun(id, overrides = {}) {
  return { id, event: 'pull_request', head_sha: sha, head_branch: 'feature/preview',
    head_repository: { id: 42, owner: { login: 'contributor' } },
    status: 'completed', conclusion: 'success',
    html_url: `https://github.com/project/reader/actions/runs/${id}`, ...overrides };
}

function fixture() {
  const state = {
    runs: { 'ci.yml': [makeRun(100)], 'browser-preview.yml': [makeRun(200)] },
    artifacts: {
      100: firmwareNames.map((name, i) => ({ name, id: 1000 + i, expired: false })),
      200: previewNames.map((name, i) => ({ name, id: 2000 + i, expired: false })),
    },
    pr: { number: 7, state: 'open', head: { sha, repo: { id: 42 } } },
    comments: [], writes: [], notices: [], reads: 0,
  };
  const context = { repo: { owner: 'project', repo: 'reader' },
    payload: { workflow_run: makeRun(100) } };
  const github = { rest: {
    pulls: {
      list: async (args) => {
        assert.equal(args.head, 'contributor:feature/preview');
        return { data: [state.pr] };
      },
      get: async () => {
        state.reads += 1;
        return { data: state.reads > 1 && state.changedPr ? state.changedPr : state.pr };
      },
    },
    actions: { listWorkflowRuns() {}, listWorkflowRunArtifacts() {} },
    issues: {
      listComments() {},
      createComment: async (args) => {
        state.writes.push({ method: 'create', ...args });
        state.comments.push({ id: 99, user: { login: 'github-actions[bot]' }, body: args.body });
      },
      updateComment: async (args) => {
        state.writes.push({ method: 'update', ...args });
        state.comments.find((comment) => comment.id === args.comment_id).body = args.body;
      },
    },
  } };
  github.paginate = async (method, args) => {
    assert.equal(args.owner, 'project');
    assert.equal(args.repo, 'reader');
    if (method === github.rest.actions.listWorkflowRuns) {
      assert.equal(args.head_sha, sha);
      assert.equal(args.event, 'pull_request');
      const result = state.runs[args.workflow_id];
      if (result instanceof Error) throw result;
      return result;
    }
    if (method === github.rest.actions.listWorkflowRunArtifacts) return state.artifacts[args.run_id];
    if (method === github.rest.issues.listComments) return state.comments;
    assert.fail('Unexpected API call');
  };
  state.execute = async (eventId = 100) => {
    context.payload.workflow_run = makeRun(eventId);
    state.reads = 0;
    await updateComment(github, context, { notice: (message) => state.notices.push(message) });
    return state.writes.at(-1)?.body;
  };
  return state;
}

test('one comment includes firmware and direct HTML artifact links for all devices', async () => {
  const state = fixture();
  const body = await state.execute();
  assert.ok(body.startsWith(marker));
  assert.match(body, /commit `abc1234`/);
  for (const name of [...firmwareNames, ...previewNames]) assert.ok(body.includes(`(\`${name}\`)](https://`));
  assert.match(body, /runs\/100\/artifacts\/1000/);
  assert.match(body, /runs\/200\/artifacts\/2002/);
  assert.match(body, /open it in Chrome/);
  assert.match(body, /14 days/);
  assert.match(body, /GitHub sign-in/);
  assert.equal(state.writes[0].method, 'create');
});

for (const [pending, first, second] of [
  ['browser-preview.yml', 100, 200], ['ci.yml', 200, 100],
]) {
  test(`both completion orders update the same comment: ${first} then ${second}`, async () => {
    const state = fixture();
    state.runs[pending][0].status = 'in_progress';
    const partial = await state.execute(first);
    assert.match(partial, /Build in progress/);
    assert.ok(partial.includes(`/runs/${first}/artifacts/`));
    assert.ok(!partial.includes(`/runs/${second}/artifacts/`));
    state.runs[pending][0].status = 'completed';
    const complete = await state.execute(second);
    assert.match(complete, /runs\/100\/artifacts/);
    assert.match(complete, /runs\/200\/artifacts/);
    assert.equal(state.writes[1].method, 'update');
    assert.equal(state.writes[1].comment_id, 99);
    assert.equal(state.comments.length, 1);
  });
}

for (const conclusion of ['failure', 'cancelled', 'timed_out']) {
  test(`${conclusion} in preview keeps firmware links and removes prior preview links`, async () => {
    const state = fixture();
    await state.execute();
    state.runs['browser-preview.yml'][0].conclusion = conclusion;
    const body = await state.execute(200);
    assert.ok(body.includes(`build finished with ${conclusion}`));
    assert.match(body, /runs\/100\/artifacts/);
    assert.doesNotMatch(body, /runs\/200\/artifacts/);
  });
}

test('firmware artifacts remain downloadable if an unrelated CI check fails', async () => {
  const state = fixture();
  state.runs['ci.yml'][0].conclusion = 'failure';
  const body = await state.execute();
  assert.match(body, /runs\/100\/artifacts/);
  assert.match(body, /CI finished with failure/);
  assert.match(body, /runs\/200\/artifacts/);
});

test('missing firmware artifacts do not prevent successful preview links', async () => {
  const state = fixture();
  state.artifacts[100].pop();
  const body = await state.execute();
  assert.match(body, /Downloads unavailable: `firmware-eego_a4.bin`/);
  assert.match(body, /runs\/200\/artifacts/);
});

for (const unavailable of ['missing', 'expired']) {
  test(`${unavailable} HTML is reported without stale links`, async () => {
    const state = fixture();
    if (unavailable === 'missing') state.artifacts[200].pop();
    else state.artifacts[200][2].expired = true;
    const body = await state.execute();
    assert.match(body, /Downloads unavailable: `crosspoint-x4pro.html`/);
    assert.doesNotMatch(body, /runs\/200\/artifacts/);
    assert.match(body, /runs\/100\/artifacts/);
  });
}

test('an old completion event cannot replace a newer queued run with old artifacts', async () => {
  const state = fixture();
  state.runs['browser-preview.yml'].push(makeRun(201, { status: 'queued', conclusion: null }));
  const body = await state.execute(200);
  assert.match(body, /Build queued/);
  assert.match(body, /runs\/201/);
  assert.doesNotMatch(body, /runs\/200/);
});

test('a rerun in progress does not expose artifacts from its earlier attempt', async () => {
  const state = fixture();
  state.runs['browser-preview.yml'][0] = makeRun(200, {
    run_attempt: 2, status: 'in_progress', conclusion: null,
  });
  const body = await state.execute(200);
  assert.match(body, /Build in progress/);
  assert.doesNotMatch(body, /runs\/200\/artifacts/);
});

test('runs from other commits, forks, branches and non-PR events are ignored', async () => {
  const state = fixture();
  state.runs['browser-preview.yml'].push(
    makeRun(300, { head_sha: 'old-commit' }),
    makeRun(301, { head_repository: { id: 43 } }),
    makeRun(302, { head_branch: 'another-branch' }),
    makeRun(303, { event: 'workflow_dispatch' }),
  );
  const body = await state.execute();
  assert.match(body, /runs\/200\/artifacts/);
  assert.doesNotMatch(body, /runs\/30[0-3]/);
});

for (const pending of [[], Object.assign(new Error('Not found'), { status: 404 })]) {
  test(`preview not started (${Array.isArray(pending) ? 'no runs' : 'workflow absent'}) keeps firmware`, async () => {
    const state = fixture();
    state.runs['browser-preview.yml'] = pending;
    const body = await state.execute();
    assert.match(body, /No build has started for this commit/);
    assert.match(body, /runs\/100\/artifacts/);
  });
}

test('unexpected API failure does not overwrite the comment with missing builds', async () => {
  const state = fixture();
  state.runs['browser-preview.yml'] = Object.assign(new Error('Forbidden'), { status: 403 });
  await assert.rejects(state.execute(), /Forbidden/);
  assert.equal(state.writes.length, 0);
});

for (const change of [{ state: 'closed' }, { head: { sha: 'new-commit', repo: { id: 42 } } },
  { head: { sha, repo: { id: 43 } } }]) {
  test(`skip a closed, superseded or unrelated PR: ${JSON.stringify(change)}`, async () => {
    const state = fixture();
    Object.assign(state.pr, change);
    await state.execute();
    assert.equal(state.writes.length, 0);
  });
}

test('skip a PR that changes while artifact metadata is being collected', async () => {
  const state = fixture();
  state.changedPr = { ...state.pr, head: { sha: 'new-commit', repo: { id: 42 } } };
  await state.execute();
  assert.equal(state.writes.length, 0);
  assert.match(state.notices[0], /PR changed/);
});

test('only the marked bot comment is updated', async () => {
  const state = fixture();
  state.comments.push(
    { id: 1, user: { login: 'contributor' }, body: marker },
    { id: 2, user: { login: 'github-actions[bot]' }, body: 'An unrelated report' },
    { id: 3, user: { login: 'github-actions[bot]' }, body: `${marker}\nOld builds` },
  );
  await state.execute();
  assert.equal(state.writes[0].comment_id, 3);
  assert.equal(state.comments[0].body, marker);
  assert.equal(state.comments[1].body, 'An unrelated report');
});
