// Fit a font's kerning into the 255 classes per side the reader can store, losing as little as
// possible: the JS twin of lib/EpdFont/scripts/kern_reduce.py, which explains the method. The two
// must give the same result (test/web/kern_reduce_test.mjs compares them), so this mirrors it step
// for step: integer arithmetic only, the same candidate order, the first cheapest candidate wins.
//
// Kerning maps are Map<leftCp * 0x110000 + rightCp, int8 value> without zero values, as
// cpfont-writer.mjs's scaleKerning() makes them.

export const KERN_CLASS_LIMIT = 255;
const KEY = 0x110000;

export function cpWeight(cp) {
  if (cp >= 0x20 && cp < 0x7F) return 16;
  if ((cp >= 0xC0 && cp <= 0x17F) || (cp >= 0x391 && cp <= 0x3C9) || (cp >= 0x410 && cp <= 0x44F)) return 4;
  return 1;
}

// Map<cp, Map<otherCp, value>> for one side: rows ('left') or columns ('right').
function profilesOf(kernMap, side) {
  const rows = new Map();
  for (const [k, value] of kernMap) {
    const left = Math.floor(k / KEY), right = k % KEY;
    const [key, other] = side === 'left' ? [left, right] : [right, left];
    if (!rows.has(key)) rows.set(key, new Map());
    rows.get(key).set(other, value);
  }
  return rows;
}

// Identical profiles grouped, numbered by their lowest code point.
function classesOf(rows) {
  const profiles = [], members = [], bySignature = new Map();
  for (const cp of [...rows.keys()].sort((a, b) => a - b)) {
    const row = rows.get(cp);
    const signature = [...row].sort((a, b) => a[0] - b[0]).map(([o, v]) => o + ':' + v).join(',');
    let index = bySignature.get(signature);
    if (index === undefined) {
      index = profiles.length;
      bySignature.set(signature, index);
      profiles.push(row);
      members.push([]);
    }
    members[index].push(cp);
  }
  return { profiles, members };
}

export function countKernClasses(kernMap) {
  return [classesOf(profilesOf(kernMap, 'left')).profiles.length,
    classesOf(profilesOf(kernMap, 'right')).profiles.length];
}

function distance(a, b) {
  let total = 0;
  for (const [key, value] of a) total += Math.abs(value - (b.get(key) ?? 0)) * cpWeight(key);
  for (const [key, value] of b) if (!a.has(key)) total += Math.abs(value) * cpWeight(key);
  return total;
}

function reduceSide(kernMap, side, cap) {
  const { profiles, members } = classesOf(profilesOf(kernMap, side));
  const n = profiles.length;
  const weight = members.map((m) => m.reduce((s, cp) => s + cpWeight(cp), 0));
  const norm = profiles.map((p) => { let s = 0; for (const [k, v] of p) s += Math.abs(v) * cpWeight(k); return s; });
  const dist = Array.from({ length: n }, () => new Float64Array(n));
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) dist[i][j] = distance(profiles[i], profiles[j]);
  const alive = new Array(n).fill(true);
  let count = n;
  while (count > cap) {
    let bestCost = null, bestFrom = -1, bestTo = -1;
    for (let i = 0; i < n; i++) {
      if (!alive[i]) continue;
      const drop = weight[i] * norm[i];  // into "no kerning"
      if (bestCost === null || drop < bestCost) { bestCost = drop; bestFrom = i; bestTo = -1; }
      for (let j = i + 1; j < n; j++) {
        if (!alive[j]) continue;
        let cost, from, to;
        if (weight[i] <= weight[j]) { cost = weight[i] * dist[i][j]; from = i; to = j; }
        else { cost = weight[j] * dist[i][j]; from = j; to = i; }
        if (cost < bestCost) { bestCost = cost; bestFrom = from; bestTo = to; }
      }
    }
    alive[bestFrom] = false;
    count--;
    if (bestTo >= 0) {
      members[bestTo].push(...members[bestFrom]);
      weight[bestTo] += weight[bestFrom];
    }
  }
  const out = new Map();
  for (let i = 0; i < n; i++) {
    if (!alive[i]) continue;
    for (const cp of members[i]) {
      for (const [other, value] of profiles[i]) out.set(side === 'left' ? cp * KEY + other : other * KEY + cp, value);
    }
  }
  return out;
}

/**
 * { map, info }: the table, merged to at most `cap` classes per side if it needed more. info is null
 * when it already fitted (and map is the one given), else { left: [before, after],
 * right: [before, after], pairsChanged }.
 */
export function reduceKernClasses(kernMap, cap = KERN_CLASS_LIMIT) {
  const [leftBefore, rightBefore] = countKernClasses(kernMap);
  if (leftBefore <= cap && rightBefore <= cap) return { map: kernMap, info: null };
  let reduced = kernMap;
  if (leftBefore > cap) reduced = reduceSide(reduced, 'left', cap);
  if (countKernClasses(reduced)[1] > cap) reduced = reduceSide(reduced, 'right', cap);
  const [leftAfter, rightAfter] = countKernClasses(reduced);
  let pairsChanged = 0;
  for (const k of new Set([...kernMap.keys(), ...reduced.keys()])) {
    if ((kernMap.get(k) ?? 0) !== (reduced.get(k) ?? 0)) pairsChanged++;
  }
  return { map: reduced, info: { left: [leftBefore, leftAfter], right: [rightBefore, rightAfter], pairsChanged } };
}
