#!/usr/bin/env node
// Timing: one whole family (4 styles x sizes 10,12,14,16,18) through cpfont-raster.mjs in Node.
//   node test/bench.mjs [--dist <dir with ftwasm.mjs>] [--family Literata] [--runs 3]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { FreeTypeWasm, rasterizeStyle, resolveIntervals } from '../js/cpfont-raster.mjs';
import { findDist, JOB_SETS } from './jobs.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = { dist: '', family: 'Literata', runs: '3' };
for (let i = 2; i < process.argv.length; i += 2) args[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];

const t0 = performance.now();
const { default: createFtWasm } = await import(pathToFileURL(path.join(findDist(ROOT, args.dist), 'ftwasm.mjs')).href);
const ft = await FreeTypeWasm.create(createFtWasm);
const initMs = performance.now() - t0;

const jobs = (args.family === 'Literata' ? JOB_SETS.literata : []);
if (!jobs.length) throw new Error('only the Literata job set is wired up');
const fonts = new Map(jobs.map((j) => [j.font, new Uint8Array(fs.readFileSync(j.font))]));
const intervals = resolveIntervals(jobs[0].intervals);

console.log(`FreeType ${ft.version().string}${ft.version().harfbuzz ? ' + HarfBuzz' : ''}; module load+init ${initMs.toFixed(1)} ms`);
for (let run = 1; run <= Number(args.runs); run++) {
  const start = performance.now();
  let glyphs = 0, bytes = 0;
  const perSize = {};
  for (const j of jobs) {
    const t = performance.now();
    const sd = rasterizeStyle(ft, fonts.get(j.font), j.size, intervals);
    perSize[j.size] = (perSize[j.size] ?? 0) + (performance.now() - t);
    glyphs += sd.glyphs.length;
    bytes += sd.totalBitmapSize;
  }
  const total = performance.now() - start;
  const sizes = Object.entries(perSize).map(([s, ms]) => `${s}pt ${ms.toFixed(0)}ms`).join(', ');
  console.log(`run ${run}: ${jobs.length} style-sizes, ${glyphs} glyphs, ${(bytes / 1024).toFixed(0)} KB bitmaps in ` +
              `${total.toFixed(0)} ms  (${sizes})`);
}
