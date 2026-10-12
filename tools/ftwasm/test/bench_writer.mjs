#!/usr/bin/env node
// Timing of the complete conversion (raster + kerning + ligatures + file layout) for one family:
// Literata, 4 styles x 10/12/14/16/18 pt, intervals as in sd-fonts.yaml.
//   node test/bench_writer.mjs [--runs 3] [--dist <dir>]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { FreeTypeWasm } from '../js/cpfont-raster.mjs';
import { convertFamily } from '../js/cpfont-writer.mjs';
import { findDist, JOB_SETS } from './jobs.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = { runs: '3', dist: '' };
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
const dist = findDist(ROOT, args.dist);

const t0 = performance.now();
const { default: createFtWasm } = await import(pathToFileURL(path.join(dist, 'ftwasm.mjs')).href);
const ft = await FreeTypeWasm.create(createFtWasm);
const initMs = performance.now() - t0;

const jobs = JOB_SETS.literata.filter((j) => j.size === 14);
const labels = ['regular', 'bold', 'italic', 'bolditalic'];
const fonts = Object.fromEntries(jobs.map((j) => [labels[j.style], new Uint8Array(fs.readFileSync(j.font))]));
console.log(`FreeType ${ft.version().string}${ft.version().harfbuzz ? ' + HarfBuzz' : ''}; module load+init ${initMs.toFixed(1)} ms`);
for (let run = 1; run <= Number(args.runs); run++) {
  const t = performance.now();
  let last = t;
  const per = [];
  const files = convertFamily(ft, {
    fonts, sizes: [10, 12, 14, 16, 18], intervals: jobs[0].intervals, name: 'Literata',
    onProgress: ({ size, done }) => { if (done % 4 === 0) { const now = performance.now(); per.push(`${size}pt ${(now - last).toFixed(0)}ms`); last = now; } },
  });
  const total = performance.now() - t;
  const bytes = files.reduce((n, f) => n + f.bytes.length, 0);
  console.log(`run ${run}: ${files.length} files, ${(bytes / 1048576).toFixed(2)} MB in ${total.toFixed(0)} ms  (${per.join(', ')})`);
}
