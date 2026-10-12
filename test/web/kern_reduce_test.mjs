// The kerning-class reduction gives the same result in the browser converter
// (tools/ftwasm/js/kern-reduce.mjs) and the Python converters (lib/EpdFont/scripts/kern_reduce.py),
// and it does its job: a table that fits is left alone, one that does not ends within the cap.
//
//   node test/web/kern_reduce_test.mjs [deps dir, unused]
//
// Needs python3 on the PATH. The tables are made up, from a fixed seed, with far more distinct rows
// and columns than a real font, so every cap below forces merges on both sides.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const { reduceKernClasses, countKernClasses } = await import(join(repo, 'tools/ftwasm/js/kern-reduce.mjs'));
const KEY = 0x110000;

let failures = 0;
const check = (ok, what) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
};

function mulberry32(seed) {
  return () => {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Code points from the ranges the weights tell apart: ASCII, Latin-1/Ext-A, Greek, Cyrillic, IPA.
function codepoints(rand, n) {
  const ranges = [[0x41, 0x7A], [0xC0, 0x17F], [0x391, 0x3C9], [0x410, 0x44F], [0x250, 0x2AF]];
  const out = new Set();
  while (out.size < n) {
    const [a, b] = ranges[Math.floor(rand() * ranges.length)];
    out.add(a + Math.floor(rand() * (b - a + 1)));
  }
  return [...out].sort((x, y) => x - y);
}

// Rows built from a handful of templates plus noise, as fonts are: groups of letters kern alike,
// with small differences that each make a class of their own.
function makeTable(seed, nLeft, nRight, templates) {
  const rand = mulberry32(seed);
  const left = codepoints(rand, nLeft), right = codepoints(rand, nRight);
  const bases = Array.from({ length: templates }, () => right.map(() => (rand() < 0.3 ? Math.round(rand() * 80 - 50) : 0)));
  const map = new Map();
  for (const l of left) {
    const base = bases[Math.floor(rand() * templates)];
    right.forEach((r, j) => {
      let v = base[j];
      if (rand() < 0.04) v += Math.round(rand() * 6 - 3);
      v = Math.max(-128, Math.min(127, v));
      if (v !== 0) map.set(l * KEY + r, v);
    });
  }
  return map;
}

function python(map, cap) {
  const script = `
import json, sys
sys.path.insert(0, ${JSON.stringify(join(repo, 'lib/EpdFont/scripts'))})
from kern_reduce import reduce_kern_classes
job = json.load(sys.stdin)
kern = {(l, r): v for l, r, v in job["map"]}
out, info = reduce_kern_classes(kern, job["cap"])
print(json.dumps({"map": sorted([l, r, v] for (l, r), v in out.items()), "info": info}))
`;
  const input = JSON.stringify({ map: [...map].map(([k, v]) => [Math.floor(k / KEY), k % KEY, v]), cap });
  const res = spawnSync('python3', ['-I', '-c', script], { input, encoding: 'utf8', maxBuffer: 1 << 28 });
  if (res.status !== 0) throw new Error('python3 failed: ' + res.stderr);
  return JSON.parse(res.stdout);
}

const entries = (map) => [...map].map(([k, v]) => [Math.floor(k / KEY), k % KEY, v])
  .sort((a, b) => a[0] - b[0] || a[1] - b[1]);

const big = makeTable(1, 380, 360, 40);
const [bigLeft, bigRight] = countKernClasses(big);
console.log(`table: ${big.size} pairs, ${bigLeft} left and ${bigRight} right classes`);
check(bigLeft > 255 && bigRight > 255, 'the made-up table needs more than 255 classes on both sides');

for (const cap of [255, 160, 64]) {
  const t = Date.now();
  const js = reduceKernClasses(big, cap);
  const jsMs = Date.now() - t;
  const py = python(big, cap);
  const [l, r] = countKernClasses(js.map);
  check(l <= cap && r <= cap, `cap ${cap}: ${l} left and ${r} right classes (JS ${jsMs} ms)`);
  check(JSON.stringify(entries(js.map)) === JSON.stringify(py.map), `cap ${cap}: JS and Python give the same table`);
  check(!!py.info && js.info.left.join() === py.info.left.join() && js.info.right.join() === py.info.right.join() &&
    js.info.pairsChanged === py.info.pairs_changed, `cap ${cap}: and the same report (${js.info.pairsChanged} pairs changed)`);
  let ascii = 0;
  for (const [k, v] of big) {
    const lc = Math.floor(k / KEY), rc = k % KEY;
    if (lc < 0x7F && rc < 0x7F && js.map.get(k) !== v) ascii++;
  }
  console.log(`      ASCII pairs changed at cap ${cap}: ${ascii}`);
}

const small = makeTable(2, 60, 50, 6);
const js = reduceKernClasses(small);
const py = python(small, 255);
check(js.map === small && js.info === null, 'a table that fits comes back untouched (JS)');
check(py.info === null && JSON.stringify(entries(small)) === JSON.stringify(py.map), 'and untouched (Python)');

console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
