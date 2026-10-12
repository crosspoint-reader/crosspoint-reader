#!/usr/bin/env node
// Parity test: fontconvert_sdcard.py (FreeType via freetype-py) vs cpfont-raster.mjs (FreeType wasm).
//
//   node test/parity.mjs [--dist <dir with ftwasm.mjs>] [--sets core,literata,wide,otf]
//                        [--python venv/bin/python] [--ref out/ref-wheel] [--regen]
//                        [--show 40] [--ascii 3] [--json out/summary.json]
//
// The Python side runs the repo's own rasterize_font_style() (test/py_dump.py);
// its dumps are cached in --ref and reused unless --regen.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { FreeTypeWasm, rasterizeStyle, resolveIntervals } from '../js/cpfont-raster.mjs';
import { findDist, jobsFor, REPO } from './jobs.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..');

function parseArgs(argv) {
  const a = { dist: '', sets: 'core', python: 'venv/bin/python', ref: 'out/ref-wheel',
              regen: false, show: 40, ascii: 0, json: '' };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i].replace(/^--/, '');
    if (k === 'regen') a.regen = true;
    else a[k] = argv[++i];
  }
  a.show = Number(a.show);
  a.ascii = Number(a.ascii);
  for (const k of ['python', 'ref']) if (!path.isAbsolute(a[k])) a[k] = path.join(ROOT, a[k]);
  a.dist = findDist(ROOT, a.dist);
  return a;
}

function runPython(args, jobs) {
  const todo = jobs.filter((j) => args.regen || !fs.existsSync(path.join(args.ref, `${j.id}.json`)));
  if (!todo.length) return;
  fs.mkdirSync(args.ref, { recursive: true });
  const jobsFile = path.join(args.ref, `_jobs-${process.pid}.json`);
  fs.writeFileSync(jobsFile, JSON.stringify(todo));
  console.log(`python: rasterising ${todo.length} job(s) into ${path.relative(ROOT, args.ref)} ...`);
  const r = spawnSync(args.python, ['-I', '-B', path.join(HERE, 'py_dump.py'), '--repo', REPO, '--jobs', jobsFile,
                                    '--out-dir', args.ref], { stdio: 'inherit' });
  fs.rmSync(jobsFile);
  if (r.status !== 0) throw new Error(`py_dump.py failed (${r.status})`);
}

const hex = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString('hex');
const cpLabel = (cp) => {
  const ch = cp >= 0x20 && !(cp >= 0x7F && cp < 0xA0) ? String.fromCodePoint(cp) : '';
  return `U+${cp.toString(16).toUpperCase().padStart(4, '0')}${ch ? ` '${ch}'` : ''}`;
};

function unpack(hexStr, w, h) {
  const bytes = Buffer.from(hexStr, 'hex');
  const px = new Uint8Array(w * h);
  for (let i = 0; i < px.length; i++) px[i] = (bytes[i >> 2] >> ((3 - (i & 3)) * 2)) & 3;
  return px;
}

function asciiPair(py, js) {
  const shade = ' .+#';
  const a = unpack(py[8], py[1], py[2]);
  const b = unpack(js.hex, js.width, js.height);
  const rows = Math.max(py[2], js.height);
  const lines = [`    python ${py[1]}x${py[2]} left=${py[4]} top=${py[5]}`.padEnd(40) +
                 `js ${js.width}x${js.height} left=${js.left} top=${js.top}`];
  for (let y = 0; y < rows; y++) {
    let l = '    ', r = '';
    for (let x = 0; x < py[1]; x++) l += y < py[2] ? shade[a[y * py[1] + x]] : ' ';
    for (let x = 0; x < js.width; x++) r += y < js.height ? shade[b[y * js.width + x]] : ' ';
    lines.push(`${l.padEnd(40)}${r}`);
  }
  return lines.join('\n');
}

function compareGlyph(py, js) {
  // py: [cp, w, h, adv, left, top, len, off, hex]
  const diffs = [];
  if (py[3] !== js.advanceX) diffs.push(`advance ${py[3]}->${js.advanceX}`);
  const sameBox = py[1] === js.width && py[2] === js.height;
  if (!sameBox) diffs.push(`size ${py[1]}x${py[2]}->${js.width}x${js.height}`);
  if (py[4] !== js.left || py[5] !== js.top) diffs.push(`origin (${py[4]},${py[5]})->(${js.left},${js.top})`);
  if (sameBox && py[8] !== js.hex) {
    const a = unpack(py[8], py[1], py[2]);
    const b = unpack(js.hex, js.width, js.height);
    let n = 0, maxd = 0;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { n++; maxd = Math.max(maxd, Math.abs(a[i] - b[i])); }
    diffs.push(`${n} px differ (max ${maxd} level${maxd > 1 ? 's' : ''})`);
  }
  return diffs;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const jobs = jobsFor(args.sets.split(','));
  runPython(args, jobs);

  const { default: createFtWasm } = await import(pathToFileURL(path.join(args.dist, 'ftwasm.mjs')).href);
  const t0 = performance.now();
  const ft = await FreeTypeWasm.create(createFtWasm);
  const initMs = performance.now() - t0;
  const v = ft.version();
  const refName = path.basename(args.ref);
  console.log(`\nwasm: ${path.relative(ROOT, args.dist)} (FreeType ${v.string}${v.harfbuzz ? ' + HarfBuzz' : ''}, init ${initMs.toFixed(0)} ms)`);

  let total = 0, matched = 0, jsMs = 0, pySec = 0;
  const mismatches = [];
  const perJob = [];
  for (const job of jobs) {
    const ref = JSON.parse(fs.readFileSync(path.join(args.ref, `${job.id}.json`), 'utf8'));
    const bytes = new Uint8Array(fs.readFileSync(job.font));
    const t = performance.now();
    const sd = rasterizeStyle(ft, bytes, job.size, resolveIntervals(job.intervals));
    const ms = performance.now() - t;
    jsMs += ms;
    pySec += ref.seconds;

    const notes = [];
    if (JSON.stringify(ref.intervals) !== JSON.stringify(sd.intervals)) notes.push('intervals differ');
    for (const k of ['advanceY', 'ascender', 'descender']) {
      if (ref[k] !== sd[k]) notes.push(`${k} ${ref[k]}->${sd[k]}`);
    }
    if (ref.totalBitmapSize !== sd.totalBitmapSize) notes.push(`bitmap bytes ${ref.totalBitmapSize}->${sd.totalBitmapSize}`);

    const jsByCp = new Map(sd.glyphs.map((g) => [g.codePoint, g]));
    let ok = 0;
    for (const py of ref.glyphs) {
      const js = jsByCp.get(py[0]);
      if (!js) { mismatches.push({ job: job.id, cp: py[0], diffs: ['missing in js'] }); continue; }
      js.hex = hex(js.bitmap);
      const diffs = compareGlyph(py, js);
      if (diffs.length === 0) ok++;
      else mismatches.push({ job: job.id, cp: py[0], diffs, py, js });
    }
    total += ref.glyphs.length;
    matched += ok;
    perJob.push({ id: job.id, glyphs: ref.glyphs.length, matched: ok, jsMs: ms, pySeconds: ref.seconds, notes });
    console.log(`${job.id.padEnd(28)} ${String(ok).padStart(5)}/${String(ref.glyphs.length).padEnd(5)} ` +
                `${(100 * ok / ref.glyphs.length).toFixed(2).padStart(6)}%  js ${ms.toFixed(0).padStart(5)} ms` +
                `  py ${ref.seconds.toFixed(2)} s${notes.length ? '  [' + notes.join('; ') + ']' : ''}`);
  }

  console.log(`\nTOTAL ${matched}/${total} glyphs identical (${(100 * matched / total).toFixed(3)} %), ` +
              `${mismatches.length} mismatching; js ${(jsMs / 1000).toFixed(2)} s, ` +
              `python ${pySec.toFixed(2)} s (python includes fontTools kerning/ligature extraction)`);
  if (mismatches.length) {
    console.log(`\nmismatches (first ${Math.min(args.show, mismatches.length)}):`);
    for (const m of mismatches.slice(0, args.show)) console.log(`  ${m.job.padEnd(28)} ${cpLabel(m.cp).padEnd(12)} ${m.diffs.join(', ')}`);
    for (const m of mismatches.slice(0, args.ascii)) {
      if (m.py) console.log(`\n  ${m.job} ${cpLabel(m.cp)}\n${asciiPair(m.py, m.js)}`);
    }
  }
  if (args.json) {
    fs.mkdirSync(path.dirname(path.resolve(args.json)), { recursive: true });
    fs.writeFileSync(args.json, JSON.stringify({
      dist: path.relative(ROOT, args.dist), freetype: v, ref: refName, total, matched, jsMs, pySeconds: pySec, perJob,
      mismatches: mismatches.map(({ job, cp, diffs }) => ({ job, cp: cpLabel(cp), diffs })),
    }, null, 1));
  }
  process.exitCode = matched === total ? 0 : 1;
}

main().catch((e) => { console.error(e); process.exit(2); });
