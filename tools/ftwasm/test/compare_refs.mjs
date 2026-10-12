#!/usr/bin/env node
// Python-vs-Python: compare two py_dump.py output dirs (e.g. wheel 2.13.2 vs native 2.13.3).
//   node test/compare_refs.mjs out/ref-wheel out/ref-native-ft2.13.3
import fs from 'node:fs';
import path from 'node:path';

const [a, b] = process.argv.slice(2);
let total = 0, same = 0, files = 0;
for (const f of fs.readdirSync(b).filter((n) => n.endsWith('.json') && !n.startsWith('_')).sort()) {
  if (!fs.existsSync(path.join(a, f))) continue;
  const A = JSON.parse(fs.readFileSync(path.join(a, f), 'utf8'));
  const B = JSON.parse(fs.readFileSync(path.join(b, f), 'utf8'));
  files++;
  const head = ['advanceY', 'ascender', 'descender'].filter((k) => A[k] !== B[k]);
  if (JSON.stringify(A.intervals) !== JSON.stringify(B.intervals)) head.push('intervals');
  const byCp = new Map(B.glyphs.map((g) => [g[0], g]));
  let ok = 0;
  const bad = [];
  for (const g of A.glyphs) {
    const h = byCp.get(g[0]);
    // compare everything except data_offset (index 7)
    if (h && [1, 2, 3, 4, 5, 6, 8].every((i) => g[i] === h[i])) ok++;
    else bad.push(`U+${g[0].toString(16).toUpperCase().padStart(4, '0')}`);
  }
  total += A.glyphs.length;
  same += ok;
  if (bad.length || head.length) console.log(`${f}: ${bad.length} glyphs differ ${bad.slice(0, 12).join(' ')} ${head.join(' ')}`);
}
console.log(`${a} vs ${b}: ${files} style-sizes, ${same}/${total} glyphs identical`);
