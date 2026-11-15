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
  if (value.image != null && (!Number.isSafeInteger(value.image) || value.image < 0)) {
    throw new Error('invalid image byte count');
  }
  if (value.base_sha != null && !/^[0-9a-f]{40}$/.test(value.base_sha)) {
    throw new Error('invalid base SHA');
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
      const stats = validateStats(decodeArtifact(data), environment, sha);
      if (stats.ram === null && stats.flash === null && stats.image == null) {
        return {reason: 'measurement contains no byte counts'};
      }
      return {stats};
    } catch (error) {
      core.warning(`Stats unavailable for ${environment}: ${error.message}`);
      return {reason: 'measurement invalid or unavailable'};
    }
  }

  const heads = [];
  for (const build of builds) heads.push(await readStats(run, build.env, run.head_sha));
  let baseline;
  let candidates = [];
  let baselineReason = 'no saved develop build';
  if (pullRequest.base.ref === 'develop') {
    try {
      baseline = heads.find(head => head.stats?.base_sha)?.stats.base_sha;
      if (!baseline) throw new Error('merge base identity missing from measurements');
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

  let body = '\n\n### Flash and static RAM usage (bytes)\n\n';
  const commit = sha => `[\`${sha.slice(0, 7)}\`](https://github.com/${owner}/${repo}/commit/${sha})`;
  body += baseline ? `Merge build for PR ${commit(run.head_sha)} compared with its develop parent ${commit(baseline)}.\n\n` : `Baseline unavailable: ${baselineReason}.\n\n`;
  body += '| Device | Baseline (Total image / linker Flash / RAM) | PR (Total image / linker Flash / RAM) | Difference (Total image / linker Flash / RAM) |\n';
  body += '| --- | ---: | ---: | ---: |\n';
  const notes = [];
  for (const [index, build] of builds.entries()) {
    const head = heads[index];
    let base = {reason: baselineReason};
    let baselineLink = '';
    for (const candidate of candidates) {
      const result = await readStats(candidate, build.env, baseline);
      if (result.stats) {
        base = result;
        baselineLink = ` <br>[CI run](${candidate.html_url})`;
        break;
      }
      if (base.reason === baselineReason) base = result;
    }
    const image = cells(base.stats, head.stats, 'image');
    const flash = cells(base.stats, head.stats, 'flash');
    const ram = cells(base.stats, head.stats, 'ram');
    const values = image.map((value, column) =>
      `${value.replace(/ B$/, '')} / ${flash[column].replace(/ B$/, '')} / ${ram[column]}`);
    body += `| ${build.device} | ${values[0]}${baselineLink} | ${values[1]} | ${values[2]} |\n`;
    if (!base.stats) notes.push(`${build.device}: baseline unavailable — ${base.reason}.`);
    if (base.stats && (base.stats.ram === null || base.stats.flash === null)) notes.push(`${build.device}: baseline measurement incomplete.`);
    if (head.stats && (head.stats.ram === null || head.stats.flash === null)) notes.push(`${build.device}: PR measurement incomplete.`);
    if (!head.stats) notes.push(`${build.device}: PR usage unavailable — ${head.reason}.`);
  }
  return body + '\n' + notes.join('\n\n');
}

module.exports = {comparison, validateStats, cells, decodeArtifact};
