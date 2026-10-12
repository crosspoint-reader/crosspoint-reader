// otf-tables.mjs -- the slice of fontTools that fontconvert_sdcard.py relies on:
// head.unitsPerEm, cmap.getBestCmap(), the legacy 'kern' table, and the GPOS/GSUB
// FeatureList + LookupList with PairPos (formats 1/2), LigatureSubst and extension
// lookups. Each reader mirrors fontTools' decoding quirks (noted inline), keyed by
// glyph id instead of glyph name (fontTools keeps names unique, so this is 1:1).
//
// No Node or DOM APIs; runs in a module Web Worker.

const tagAt = (dv, off) => String.fromCharCode(dv.getUint8(off), dv.getUint8(off + 1), dv.getUint8(off + 2), dv.getUint8(off + 3));

export class FontError extends Error {}

export class SfntFont {
  /** @param {Uint8Array} bytes  @param {string} [label] for error messages */
  constructor(bytes, label = 'font') {
    this.bytes = bytes;
    this.label = label;
    this.dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (bytes.byteLength < 12) throw new FontError(`${label}: not an OpenType/TrueType font (too short)`);
    const sig = tagAt(this.dv, 0);
    if (sig === 'ttcf') throw new FontError(`${label}: TrueType/OpenType collections (.ttc/.otc) are not supported; extract a single font first`);
    if (sig === 'wOFF' || sig === 'wOF2') throw new FontError(`${label}: WOFF/WOFF2 web fonts are not supported; convert to .ttf/.otf first`);
    const ver = this.dv.getUint32(0);
    if (ver !== 0x00010000 && sig !== 'OTTO' && sig !== 'true') throw new FontError(`${label}: not an OpenType/TrueType font`);
    this.flavor = sig === 'OTTO' ? 'cff' : 'truetype';
    const numTables = this.dv.getUint16(4);
    this.tables = new Map();
    for (let i = 0; i < numTables; i++) {
      const rec = 12 + i * 16;
      const tag = tagAt(this.dv, rec);
      const offset = this.dv.getUint32(rec + 8);
      const length = this.dv.getUint32(rec + 12);
      this.tables.set(tag, { offset, length });
    }
  }

  has(tag) { return this.tables.has(tag); }

  /** DataView over one table (clipped to the file, as a file read would be). */
  table(tag) {
    const t = this.tables.get(tag);
    if (!t) return null;
    const start = Math.min(t.offset, this.bytes.byteLength);
    const end = Math.min(t.offset + t.length, this.bytes.byteLength);
    return new DataView(this.bytes.buffer, this.bytes.byteOffset + start, end - start);
  }

  get isVariable() { return this.has('fvar'); }

  unitsPerEm() {
    const head = this.table('head');
    if (!head) throw new FontError(`${this.label}: no 'head' table`);
    return head.getUint16(18);
  }

  numGlyphs() {
    const maxp = this.table('maxp');
    return maxp ? maxp.getUint16(4) : 0;
  }

  /**
   * fontTools getBestCmap(): first subtable (in file order) for the first of
   * (3,10) (0,6) (0,4) (3,1) (0,3) (0,2) (0,1) (0,0); gid 0 entries dropped;
   * a repeated code keeps its first position but takes the last gid (dict semantics).
   * Returns Map<codepoint, gid> in fontTools' iteration order (empty Map if none).
   */
  bestCmap() {
    if (this._bestCmap) return this._bestCmap;
    const dv = this.table('cmap');
    const result = new Map();
    this._bestCmap = result;
    if (!dv) return result;
    const n = dv.getUint16(2);
    const subtables = [];
    for (let i = 0; i < n; i++) {
      const platformID = dv.getUint16(4 + i * 8);
      const platEncID = dv.getUint16(6 + i * 8);
      const offset = dv.getInt32(8 + i * 8);
      const format = dv.getUint16(offset);
      let length;
      if ([8, 10, 12, 13].includes(format)) length = dv.getUint32(offset + 4);
      else if (format === 14) length = dv.getUint32(offset + 2);
      else length = dv.getUint16(offset + 2);
      if (!length) continue; // fontTools skips zero-length subtables
      subtables.push({ platformID, platEncID, offset, format, length });
    }
    const prefs = [[3, 10], [0, 6], [0, 4], [3, 1], [0, 3], [0, 2], [0, 1], [0, 0]];
    let st = null;
    for (const [p, e] of prefs) {
      st = subtables.find((s) => s.platformID === p && s.platEncID === e);
      if (st) break;
    }
    if (!st) return result;
    const end = Math.min(st.offset + st.length, dv.byteLength);
    const sub = new DataView(dv.buffer, dv.byteOffset + st.offset, end - st.offset);
    const put = (code, gid) => { if (gid !== 0) result.set(code, gid); };
    switch (st.format) {
      case 0: {
        if (st.length !== 262) throw new FontError(`${this.label}: format 0 cmap subtable not 262 bytes`);
        for (let c = 0; c < 256; c++) put(c, sub.getUint8(6 + c));
        break;
      }
      case 4: {
        // fontTools: words after the 14-byte header, last segment skipped
        // ("don't do 0xffff"), idDelta read unsigned and masked.
        const words = (sub.byteLength - 14) >> 1;
        const w = (k) => sub.getUint16(14 + 2 * k);
        const segCount = sub.getUint16(6) >> 1;
        const endAt = 0, startAt = segCount + 1, deltaAt = 2 * segCount + 1, rangeAt = 3 * segCount + 1, giaAt = 4 * segCount + 1;
        const lenGIA = Math.max(0, words - giaAt);
        for (let i = 0; i < segCount - 1; i++) {
          const start = w(startAt + i), endc = w(endAt + i), delta = w(deltaAt + i), rangeOffset = w(rangeAt + i);
          const partial = Math.floor(rangeOffset / 2) - start + i - segCount;
          for (let c = start; c <= endc; c++) {
            if (rangeOffset === 0) put(c, (c + delta) & 0xFFFF);
            else {
              const index = c + partial;
              if (!(index < lenGIA)) throw new FontError(`${this.label}: format 4 cmap index out of range`);
              const g = w(giaAt + index);
              put(c, g !== 0 ? (g + delta) & 0xFFFF : 0);
            }
          }
        }
        break;
      }
      case 6: {
        const first = sub.getUint16(6), count = sub.getUint16(8);
        const avail = Math.min(count, (sub.byteLength - 10) >> 1);
        for (let k = 0; k < avail; k++) put(first + k, sub.getUint16(10 + 2 * k));
        break;
      }
      case 12:
      case 13: {
        const nGroups = sub.getUint32(12);
        if (sub.byteLength !== 16 + nGroups * 12 || st.length !== sub.byteLength) {
          throw new FontError(`${this.label}: corrupt cmap table format ${st.format}`);
        }
        for (let g = 0; g < nGroups; g++) {
          const s = sub.getUint32(16 + g * 12), e = sub.getUint32(20 + g * 12), gid = sub.getUint32(24 + g * 12);
          for (let c = s; c <= e; c++) put(c, st.format === 12 ? gid + (c - s) : gid);
        }
        break;
      }
      default:
        throw new FontError(`${this.label}: cmap subtable format ${st.format} is not supported by the browser converter`);
    }
    return result;
  }

  /**
   * Legacy 'kern' as fontTools exposes it: a list of format-0 subtables, each a
   * Map<(left << 16 | right), value>. Apple version-1 headers are read; other
   * subtable formats yield no pairs; coverage bits are ignored. A table with a
   * single format-0 subtable takes its length from nPairs, not the length field.
   */
  kernSubtables() {
    const dv = this.table('kern');
    if (!dv) return [];
    let version = dv.getUint16(0), nTables = dv.getUint16(2), pos = 4, apple = false;
    if (dv.byteLength >= 8 && version === 1) {
      nTables = dv.getUint32(4);
      pos = 8;
      apple = true;
    }
    const out = [];
    for (let i = 0; i < nTables; i++) {
      let length, format;
      if (apple) {
        length = dv.getUint32(pos);
        format = dv.getUint8(pos + 5);
      } else {
        length = dv.getUint16(pos + 2);
        format = dv.getUint8(pos + 4);
        if (nTables === 1 && format === 0) length = dv.getUint16(pos + 6) * 6 + 14;
      }
      if (format === 0) {
        let p = pos;
        if (!apple) {
          if (dv.getUint16(p) !== 0) throw new FontError(`${this.label}: unsupported kern subtable version`);
          p += 6;
        } else p += 8;
        const nPairs = dv.getUint16(p);
        p += 8;
        const pairs = new Map();
        for (let k = 0; k < nPairs; k++) {
          const q = p + k * 6;
          pairs.set(dv.getUint16(q) * 65536 + dv.getUint16(q + 2), dv.getInt16(q + 4));
        }
        out.push(pairs);
      }
      pos += length;
    }
    return out;
  }

  /** GPOS/GSUB reader, or null when the table is absent. */
  layout(tag) {
    const dv = this.table(tag);
    return dv ? new LayoutTable(dv, tag, this.label) : null;
  }
}

// ---------------------------------------------------------------------------
// GPOS / GSUB

/** fontTools Coverage.glyphs: format 2 ranges stable-sorted by StartCoverageIndex, then expanded. */
export function readCoverage(dv, off) {
  const format = dv.getUint16(off);
  if (format === 1) {
    const n = dv.getUint16(off + 2);
    const glyphs = new Array(n);
    for (let i = 0; i < n; i++) glyphs[i] = dv.getUint16(off + 4 + 2 * i);
    return glyphs;
  }
  if (format === 2) {
    const n = dv.getUint16(off + 2);
    const ranges = [];
    for (let i = 0; i < n; i++) {
      const r = off + 4 + 6 * i;
      ranges.push({ start: dv.getUint16(r), end: dv.getUint16(r + 2), sci: dv.getUint16(r + 4) });
    }
    ranges.sort((a, b) => a.sci - b.sci); // Array.prototype.sort is stable, like sorted()
    const glyphs = [];
    for (const r of ranges) for (let g = r.start; g <= r.end; g++) glyphs.push(g);
    return glyphs;
  }
  return []; // fontTools: "Unknown Coverage format" -> empty
}

/** fontTools ClassDef.classDefs as Map<gid, class>; class-0 entries never stored. */
export function readClassDef(dv, off) {
  const map = new Map();
  if (!off) return map;
  const format = dv.getUint16(off);
  if (format === 1) {
    const start = dv.getUint16(off + 2), n = dv.getUint16(off + 4);
    for (let i = 0; i < n; i++) {
      const cls = dv.getUint16(off + 6 + 2 * i);
      if (cls) map.set(start + i, cls);
    }
  } else if (format === 2) {
    const n = dv.getUint16(off + 2);
    for (let i = 0; i < n; i++) {
      const r = off + 4 + 6 * i;
      const s = dv.getUint16(r), e = dv.getUint16(r + 2), cls = dv.getUint16(r + 4);
      if (!cls) continue;
      for (let g = s; g <= e; g++) map.set(g, cls);
    }
  }
  return map;
}

const popcount16 = (v) => { let c = 0; for (v &= 0xFFFF; v; v &= v - 1) c++; return c; };

/** XAdvance of a ValueRecord at `off` with `format`, or 0 when absent (fontTools `... or 0`). */
function xAdvance(dv, off, format) {
  if (!(format & 0x0004)) return 0;
  return dv.getInt16(off + 2 * popcount16(format & 0x0003));
}

export class LayoutTable {
  constructor(dv, tag, label) {
    this.dv = dv;
    this.tag = tag;
    this.label = label;
    this.extType = tag === 'GPOS' ? 9 : 7;
    this.featureListOff = dv.getUint16(6);
    this.lookupListOff = dv.getUint16(8);
  }

  /** FeatureRecords in file order: [{tag, lookupIndices}] (fontTools ignores the ScriptList here). */
  features() {
    const dv = this.dv, fl = this.featureListOff;
    if (!fl) return [];
    const n = dv.getUint16(fl);
    const out = [];
    for (let i = 0; i < n; i++) {
      const rec = fl + 2 + 6 * i;
      const tag = tagAt(dv, rec);
      const fo = fl + dv.getUint16(rec + 4);
      const cnt = dv.getUint16(fo + 2);
      const idx = [];
      for (let k = 0; k < cnt; k++) idx.push(dv.getUint16(fo + 4 + 2 * k));
      out.push({ tag, lookupIndices: idx });
    }
    return out;
  }

  lookupCount() { return this.lookupListOff ? this.dv.getUint16(this.lookupListOff) : 0; }

  /**
   * One lookup: {type, subtables: [{type, off}]} where extension subtables
   * (GPOS 9 / GSUB 7) are unwrapped to their ExtensionLookupType and target.
   */
  lookup(li) {
    const dv = this.dv, ll = this.lookupListOff;
    if (!ll || li >= dv.getUint16(ll)) throw new FontError(`${this.label}: ${this.tag} lookup index ${li} out of range`);
    const lo = ll + dv.getUint16(ll + 2 + 2 * li);
    const type = dv.getUint16(lo);
    const n = dv.getUint16(lo + 4);
    const subtables = [];
    for (let i = 0; i < n; i++) {
      const so = dv.getUint16(lo + 6 + 2 * i);
      if (!so) continue;
      const off = lo + so;
      if (type === this.extType) {
        subtables.push({ type: dv.getUint16(off + 2), off: off + dv.getUint32(off + 4) });
      } else subtables.push({ type, off });
    }
    return { type, subtables };
  }

  /**
   * PairPos subtable -> calls emit(leftGid, rightGid, xAdvance) for every pair the
   * Python extractor would add, with its filters: left must be in `wanted`; for
   * format 1 right must be in `wanted`; format 2 walks classes like the script
   * (left glyphs = wanted ∩ coverage by ClassDef1, right = all wanted by ClassDef2).
   * `wanted` is an ordered iterable of gids (Map keys / Set).
   * Returns the set of (left,right) keys this subtable "matched" in OpenType's
   * first-match sense when `matched` is given (used by the opt-in fix only).
   */
  pairPos(off, wanted, emit, matched = null) {
    const dv = this.dv;
    const format = dv.getUint16(off);
    const coverage = readCoverage(dv, off + dv.getUint16(off + 2));
    const vf1 = dv.getUint16(off + 4), vf2 = dv.getUint16(off + 6);
    const rec1 = 2 * popcount16(vf1), rec2 = 2 * popcount16(vf2);
    if (format === 1) {
      const setCount = dv.getUint16(off + 8);
      for (let i = 0; i < coverage.length; i++) {
        const lg = coverage[i];
        if (!wanted.has(lg)) continue;
        if (i >= setCount) throw new FontError(`${this.label}: PairPos format 1 coverage longer than PairSet list`);
        const ps = off + dv.getUint16(off + 10 + 2 * i);
        const cnt = dv.getUint16(ps);
        for (let k = 0; k < cnt; k++) {
          const r = ps + 2 + k * (2 + rec1 + rec2);
          const rg = dv.getUint16(r);
          if (!wanted.has(rg)) continue;
          const key = lg * 65536 + rg;
          if (matched) {
            if (matched.has(key)) continue;
            matched.add(key);
          }
          const xa = xAdvance(dv, r + 2, vf1);
          if (xa !== 0) emit(lg, rg, xa);
        }
      }
    } else if (format === 2) {
      const cd1 = readClassDef(dv, dv.getUint16(off + 8) ? off + dv.getUint16(off + 8) : 0);
      const cd2 = readClassDef(dv, dv.getUint16(off + 10) ? off + dv.getUint16(off + 10) : 0);
      const c1n = dv.getUint16(off + 12), c2n = dv.getUint16(off + 14);
      const covSet = new Set(coverage);
      const leftByClass = new Map();
      for (const g of wanted.keys()) {
        if (!covSet.has(g)) continue;
        const c = cd1.get(g) ?? 0;
        if (!leftByClass.has(c)) leftByClass.set(c, []);
        leftByClass.get(c).push(g);
      }
      const rightByClass = new Map();
      for (const g of wanted.keys()) {
        const c = cd2.get(g) ?? 0;
        if (!rightByClass.has(c)) rightByClass.set(c, []);
        rightByClass.get(c).push(g);
      }
      const recSize = rec1 + rec2;
      for (let c1 = 0; c1 < c1n; c1++) {
        const lefts = leftByClass.get(c1);
        if (!lefts) continue;
        for (let c2 = 0; c2 < c2n; c2++) {
          const r = off + 16 + (c1 * c2n + c2) * recSize;
          const xa = xAdvance(dv, r, vf1);
          const rights = rightByClass.get(c2);
          if (matched) {
            // OpenType: a covered first glyph matches this subtable for every second glyph.
            if (!rights) continue;
            for (const lg of lefts) for (const rg of rights) {
              const key = lg * 65536 + rg;
              if (matched.has(key)) continue;
              matched.add(key);
              if (xa !== 0) emit(lg, rg, xa);
            }
            continue;
          }
          if (xa === 0 || !rights) continue;
          for (const lg of lefts) for (const rg of rights) emit(lg, rg, xa);
        }
      }
    }
  }

  /**
   * LigatureSubst format 1 -> Map<firstGid, [{ligGlyph, components: [gid...]}]>
   * with fontTools' dict semantics (repeated coverage glyph: first position, last value).
   */
  ligatureSubst(off) {
    const dv = this.dv;
    const format = dv.getUint16(off);
    if (format !== 1) throw new FontError(`${this.label}: unknown LigatureSubst format ${format}`);
    const coverage = readCoverage(dv, off + dv.getUint16(off + 2));
    const n = dv.getUint16(off + 4);
    if (coverage.length !== n) throw new FontError(`${this.label}: LigatureSubst coverage/LigatureSet count mismatch`);
    const out = new Map();
    for (let i = 0; i < n; i++) {
      const ls = off + dv.getUint16(off + 6 + 2 * i);
      const cnt = dv.getUint16(ls);
      const ligs = [];
      for (let k = 0; k < cnt; k++) {
        const lo = ls + dv.getUint16(ls + 2 + 2 * k);
        const ligGlyph = dv.getUint16(lo);
        const compCount = dv.getUint16(lo + 2);
        const components = [];
        for (let c = 0; c < compCount - 1; c++) components.push(dv.getUint16(lo + 4 + 2 * c));
        ligs.push({ ligGlyph, components });
      }
      out.set(coverage[i], ligs);
    }
    return out;
  }
}
