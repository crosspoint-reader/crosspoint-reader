const {execFileSync} = require('node:child_process');

// Read one bounded JSON member in memory. Never extract or execute PR artifacts.
function decodeArtifact(data) {
  if (data.byteLength > 65536) throw new Error('oversized stats archive');
  return JSON.parse(execFileSync('python3', ['-c', `
import io, sys, zipfile
with zipfile.ZipFile(io.BytesIO(sys.stdin.buffer.read())) as archive:
    entries = archive.infolist()
    if len(entries) != 1 or entries[0].filename != 'firmware-stats.json' or entries[0].file_size > 4096:
        raise ValueError('invalid stats archive')
    sys.stdout.buffer.write(archive.read(entries[0]))
`], {input: Buffer.from(data), maxBuffer: 8192}).toString());
}

function validateStats(value, environment, sha) {
  if (value?.environment !== environment || value.sha !== sha) throw new Error('stats identity mismatch');
  for (const key of ['ram', 'flash']) {
    if (value[key] !== null && (!Number.isSafeInteger(value[key]) || value[key] < 0)) {
      throw new Error('invalid byte count');
    }
  }
  return value;
}

function cells(base, head, key) {
  const bytes = value => value == null ? 'unavailable' : `${value.toLocaleString('en-US')} B`;
  const delta = base?.[key] != null && head?.[key] != null ? head[key] - base[key] : null;
  return [bytes(base?.[key]), bytes(head?.[key]), delta === null ? 'unavailable' : `${delta > 0 ? '+' : ''}${bytes(delta)}`];
}

async function comparison({github, context, core, run, pullRequest, builds}) {
  const {owner, repo} = context.repo;
  const api = {owner, repo};
  let baseline;
  let candidates = [];
  let baselineReason = 'no saved develop build';
  if (pullRequest.base.ref === 'develop') {
    try {
      const {data} = await github.rest.repos.compareCommitsWithBasehead({
        ...api, basehead: `develop...${run.head_sha}`, per_page: 1,
      });
      baseline = data.merge_base_commit.sha;
      candidates = await github.paginate(github.rest.actions.listWorkflowRuns, {
        ...api, workflow_id: run.workflow_id, event: 'push', branch: 'develop',
        head_sha: baseline, status: 'completed', per_page: 100,
      });
      candidates = candidates.filter(r => r.head_sha === baseline && r.event === 'push' && r.head_branch === 'develop')
        .sort((a, b) => b.id - a.id || b.run_attempt - a.run_attempt);
    } catch (error) {
      core.warning(`Baseline lookup unavailable: ${error.message}`);
      baselineReason = 'baseline lookup failed';
    }
  } else baselineReason = 'PR does not target develop';

  const artifactCache = new Map();
  async function readStats(buildRun, environment, sha) {
    try {
      if (!artifactCache.has(buildRun.id)) {
        artifactCache.set(buildRun.id, await github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
          ...api, run_id: buildRun.id, per_page: 100,
        }));
      }
      const artifact = artifactCache.get(buildRun.id).find(a => a.name === `firmware-stats-${environment}`);
      if (!artifact) return {reason: 'measurement missing (build may have failed)'};
      if (artifact.expired) return {reason: 'measurement expired'};
      if (artifact.size_in_bytes > 65536) return {reason: 'invalid measurement'};
      const {data} = await github.rest.actions.downloadArtifact({...api, artifact_id: artifact.id, archive_format: 'zip'});
      return {stats: validateStats(decodeArtifact(data), environment, sha)};
    } catch (error) {
      core.warning(`Stats unavailable for ${environment}: ${error.message}`);
      return {reason: 'measurement invalid or unavailable'};
    }
  }

  let body = '\n\n### Flash and static RAM usage\n\n';
  const commit = sha => `[\`${sha.slice(0, 7)}\`](https://github.com/${owner}/${repo}/commit/${sha})`;
  body += baseline ? `PR ${commit(run.head_sha)} compared with shared ancestor ${commit(baseline)} in develop.\n\n` : `Baseline unavailable: ${baselineReason}.\n\n`;
  body += '| Device | Flash baseline | Flash PR | Flash difference | RAM baseline | RAM PR | RAM difference |\n';
  body += '| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n';
  const notes = [];
  for (const build of builds) {
    const head = await readStats(run, build.env, run.head_sha);
    let base = {reason: baselineReason};
    for (const candidate of candidates) {
      const result = await readStats(candidate, build.env, baseline);
      if (result.stats) {
        base = result;
        notes.push(`${build.device} baseline: [CI run](${candidate.html_url}).`);
        break;
      }
      if (base.reason === baselineReason) base = result;
    }
    const values = [...cells(base.stats, head.stats, 'flash'), ...cells(base.stats, head.stats, 'ram')];
    body += `| ${build.device} | ${values.join(' | ')} |\n`;
    if (!base.stats) notes.push(`${build.device}: baseline unavailable — ${base.reason}.`);
    if (base.stats && (base.stats.ram === null || base.stats.flash === null)) notes.push(`${build.device}: baseline measurement incomplete.`);
    if (head.stats && (head.stats.ram === null || head.stats.flash === null)) notes.push(`${build.device}: PR measurement incomplete.`);
    if (!head.stats) notes.push(`${build.device}: PR usage unavailable — ${head.reason}.`);
  }
  return body + '\nDifferences are PR minus baseline. RAM is static allocation; runtime heap use is not measured.\n\n' + notes.join('\n\n');
}

module.exports = {comparison, validateStats, cells, decodeArtifact};
