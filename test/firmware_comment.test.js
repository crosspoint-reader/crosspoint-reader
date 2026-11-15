const {test} = require('node:test');
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const {comparison, cells, validateStats, decodeArtifact} = require('../.github/scripts/firmware-size-comparison');
const publish = require('../.github/scripts/pr-firmware-comment');
const sha = 'a'.repeat(40), baseline = 'b'.repeat(40);
const builds = [{env: 'default', device: 'X3 / X4'}];
function zip(stats) {
  return execFileSync('python3', ['-c', 'import io,sys,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,"w"); z.writestr("firmware-stats.json",sys.stdin.read()); z.close(); sys.stdout.buffer.write(b.getvalue())'], {input: JSON.stringify(stats)});
}
function fixture() {
  const run = {id: 20, workflow_id: 5, head_sha: sha, head_branch: 'feature', head_repository: {id: 2, owner: {login: 'contributor'}}, run_attempt: 1, conclusion: 'success'};
  const pullRequest = {number: 10, state: 'open', head: {sha}, base: {ref: 'develop'}};
  const records = {20: [{name: 'firmware-stats-default', id: 20}], 10: [{name: 'firmware-stats-default', id: 10}]};
  const payloads = {20: zip({environment: 'default', sha, ram: 90, flash: 130}), 10: zip({environment: 'default', sha: baseline, ram: 100, flash: 120})};
  let runs = [{id: 10, head_sha: baseline, event: 'push', head_branch: 'develop', html_url: 'https://example.test/run/10'}];
  const posted = [];
  const actions = {
    listWorkflowRuns: 'runs', listWorkflowRunArtifacts: 'artifacts',
    downloadArtifact: async ({artifact_id}) => ({data: payloads[artifact_id]}),
  };
  const github = {
    rest: {actions, repos: {compareCommitsWithBasehead: async ({basehead}) => {
      assert.equal(basehead, `develop...${sha}`);
      return {data: {merge_base_commit: {sha: baseline}}};
    }}, pulls: {list: 'pulls', get: async () => ({data: pullRequest})},
    issues: {listComments: 'comments', createComment: async value => posted.push(value), updateComment: async value => posted.push(value)}},
    paginate: async (method, args) => {
      if (method === 'artifacts') return records[args.run_id] || [];
      if (method === 'pulls') return [pullRequest];
      if (method === 'comments') return [];
      if (method === 'runs') return args.event === 'push' ? runs : [run];
      throw new Error(method);
    },
  };
  return {github, context: {repo: {owner: 'upstream', repo: 'reader'}, payload: {workflow_run: run}}, core: {notice() {}, warning() {}}, run, pullRequest, builds, records, posted,
    setRuns(value) {runs = value;}};
}

test('exact byte deltas and unavailable values', () => {
  assert.deepEqual(cells({ram: 100}, {ram: 90}, 'ram'), ['100 B', '90 B', '-10 B']);
  assert.equal(cells({ram: 100}, {ram: 110}, 'ram')[2], '+10 B');
  assert.equal(cells({ram: 100}, {ram: 100}, 'ram')[2], '0 B');
  assert.equal(cells(null, {ram: 0}, 'ram')[2], 'unavailable');
});
test('bounded archives and identity validation', () => {
  assert.throws(() => decodeArtifact(Buffer.alloc(65537)));
  assert.throws(() => validateStats({environment: 'default', sha, ram: -1, flash: 1}, 'default', sha));
  assert.throws(() => validateStats({environment: 'default', sha, ram: 1, flash: 1}, 'default', baseline));
  assert.throws(() => validateStats({environment: 'default', sha, ram: '1', flash: 1}, 'default', sha));
});
test('exact baseline and per-board values', async () => {
  const body = await comparison(fixture());
  assert.match(body, /\+10 B/); assert.match(body, /-10 B/); assert.match(body, /bbbbbbb/);
});
test('expired baseline and missing head are explicit', async () => {
  const f = fixture(); f.records[10][0].expired = true; f.records[20] = [];
  const body = await comparison(f);
  assert.match(body, /measurement expired/); assert.match(body, /PR usage unavailable/);
});
test('never substitute another SHA; try older exact runs if newer has no measurement', async () => {
  const f = fixture();
  f.setRuns([{id: 99, head_sha: sha, event: 'push', head_branch: 'develop'},
    {id: 11, head_sha: baseline, event: 'push', head_branch: 'develop'},
    {id: 10, head_sha: baseline, event: 'push', head_branch: 'develop'}]);
  assert.match(await comparison(f), /\+10 B/);
});
test('no baseline run and non-develop targets', async () => {
  const f = fixture(); f.setRuns([]);
  assert.match(await comparison(f), /no saved develop build/);
  f.pullRequest.base.ref = 'release';
  assert.match(await comparison(f), /does not target develop/);
});
test('publisher retains partial downloads and creates one comment', async () => {
  const f = fixture(); f.records[20].push({name: 'firmware.bin', id: 30});
  await publish(f);
  assert.equal(f.posted.length, 1);
  assert.match(f.posted[0].body, /artifacts\/30/);
  assert.match(f.posted[0].body, /Flash and static RAM usage/);
});
test('closed PRs and superseded heads do not publish', async () => {
  for (const change of [p => p.state = 'closed', p => p.head.sha = baseline]) {
    const f = fixture(); change(f.pullRequest); await publish(f); assert.equal(f.posted.length, 0);
  }
});
test('older completion and older rerun attempt do not publish', async () => {
  for (const newer of [{id: 21}, {run_attempt: 2}]) {
    const f = fixture(); const paginate = f.github.paginate;
    f.github.paginate = async (method, args) => method === 'runs' && args.event === 'pull_request'
      ? [{...f.run, ...newer}] : paginate(method, args);
    await publish(f); assert.equal(f.posted.length, 0);
  }
});

test('updates the existing bot comment and rechecks head before writing', async () => {
  const f = fixture();
  const paginate = f.github.paginate;
  f.github.paginate = async (method, args) => method === 'comments'
    ? [{id: 42, user: {login: 'github-actions[bot]'}, body: '<!-- pr-firmware-artifacts -->'}]
    : paginate(method, args);
  await publish(f);
  assert.equal(f.posted[0].comment_id, 42);
  f.posted.length = 0;
  let gets = 0;
  f.github.rest.pulls.get = async () => ({data: ++gets === 1 ? f.pullRequest : {...f.pullRequest, head: {sha: baseline}}});
  await publish(f);
  assert.equal(f.posted.length, 0);
});

test('each publication recomputes merge-base after rebases and merges', async () => {
  const f = fixture();
  for (const newBase of ['c'.repeat(40), 'd'.repeat(40)]) {
    f.github.rest.repos.compareCommitsWithBasehead = async () => ({data: {merge_base_commit: {sha: newBase}}});
    f.setRuns([]);
    assert.match(await comparison(f), new RegExp(newBase.slice(0, 7)));
  }
});
