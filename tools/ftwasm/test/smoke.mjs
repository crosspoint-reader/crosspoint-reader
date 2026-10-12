#!/usr/bin/env node
// Loads the module the way a page/worker would (wasm bytes handed in, no filesystem),
// forces wasm memory growth mid-run, and checks the output does not change.
//   node test/smoke.mjs [<dir with ftwasm.mjs>]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { FreeTypeWasm, rasterizeStyle, resolveIntervals } from '../js/cpfont-raster.mjs';
import { findDist, JOB_SETS } from './jobs.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dist = findDist(ROOT, process.argv[2]);
const { default: createFtWasm } = await import(pathToFileURL(path.join(dist, 'ftwasm.mjs')).href);
const ft = await FreeTypeWasm.create(createFtWasm, { wasmBinary: fs.readFileSync(path.join(dist, 'ftwasm.wasm')) });
const job = JOB_SETS.core[0];
const font = new Uint8Array(fs.readFileSync(job.font));
const iv = resolveIntervals(job.intervals);
const a = rasterizeStyle(ft, font, job.size, iv);
const heapBefore = ft.m.HEAPU8.length;
if (!ft.m._malloc(200 << 20)) throw new Error('malloc for growth failed');
const b = rasterizeStyle(ft, font, job.size, iv);
const same = Buffer.compare(Buffer.from(a.bitmap), Buffer.from(b.bitmap)) === 0 &&
             JSON.stringify(a.glyphs.map(({ bitmap, ...g }) => g)) === JSON.stringify(b.glyphs.map(({ bitmap, ...g }) => g));
const face = ft.openFace(font);
const info = face.faceInfo();
face.done();
console.log(`FreeType ${ft.version().string}${ft.version().harfbuzz ? ' + HarfBuzz' : ''}; ${info.familyName} ${info.styleName}, ` +
            `${a.glyphs.length} glyphs; heap ${heapBefore} -> ${ft.m.HEAPU8.length}; identical after growth: ${same}`);
process.exitCode = same ? 0 : 1;
