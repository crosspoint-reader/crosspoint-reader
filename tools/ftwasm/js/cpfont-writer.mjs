// cpfont-writer.mjs -- the rest of fontconvert_sdcard.py: kerning, ligatures,
// synthetic bold and the v4 .cpfont file layout, on top of cpfont-raster.mjs.
//
//   const ft = await FreeTypeWasm.create(createFtWasm);
//   const files = convertFamily(ft, {
//     fonts: { regular: bytesR, bold: bytesB, italic: bytesI, bolditalic: bytesBI },
//     sizes: [10, 12, 14, 16, 18],
//     intervals: 'builtin,latin-cyrillic,greek-letters,musical-symbols',
//     name: 'Literata',
//   });
//   // -> [{ fileName: 'Literata_10.cpfont', size: 10, bytes: Uint8Array }, ...]
//
// Output is byte-identical to
//   fontconvert_sdcard.py --regular R --bold B --italic I --bolditalic BI \
//     --intervals <set> --sizes <list> --name <name> --output-dir <dir>/
// which is how build-sd-fonts.py invokes it for every family in sd-fonts.yaml.
// That includes the script's current quirks (kerning summed over all subtables
// plus the legacy 'kern' table, Python round-half-to-even, CPython set order
// for ligature lookups). The OpenType first-match kerning fix is available as
// `kerning: 'first-match'` and is off by default.
//
// No Node or DOM APIs; runs in a module Web Worker.

import { FontError, SfntFont } from './otf-tables.mjs';
import { PyIntSet } from './pyset-order.mjs';
import { reduceKernClasses } from './kern-reduce.mjs';
import { rasterizeStyle, resolveIntervals, CONVERTER_DPI } from './cpfont-raster.mjs';

export { FontError };

export const STYLE_IDS = { regular: 0, bold: 1, italic: 2, bolditalic: 3 };
export const STYLE_LABELS = ['regular', 'bold', 'italic', 'bolditalic'];

// --- numeric helpers ------------------------------------------------------

/** Python's round() on a float: nearest integer, ties to even. */
export function roundHalfEven(x) {
  const r = Math.round(x);
  return Math.abs(x % 1) === 0.5 ? 2 * Math.round(x / 2) : r;
}

/** fp4_from_design_units(): design units -> 4.4 fixed point, clamped to int8. */
export function fp4FromDesignUnits(du, scale) {
  const raw = roundHalfEven(du * scale * 16);
  return Math.max(-128, Math.min(127, raw));
}

// --- kerning --------------------------------------------------------------

/**
 * The size-independent half of extract_kerning_fonttools(): design-unit sums per
 * glyph pair. `codepoints` = every emitted glyph's codepoint (all_cps).
 * mode 'sum' (default, what the script does): add every format-0 'kern' pair and
 * every GPOS 'kern' PairPos value, across all lookups and all subtables.
 * mode 'first-match' (opt-in fix): within a lookup only the first subtable that
 * matches a pair counts (OpenType/HarfBuzz rule), and the legacy 'kern' table
 * is used only when GPOS has no 'kern' feature.
 */
export function prepareKerning(font, codepoints, mode = 'sum') {
  const cmap = font.bestCmap();
  const glyphToCps = new Map(); // gid -> [cp...]
  for (const cp of codepoints) {
    const gid = cmap.get(cp);
    if (gid) {
      if (!glyphToCps.has(gid)) glyphToCps.set(gid, []);
      glyphToCps.get(gid).push(cp);
    }
  }
  const raw = new Map(); // lg*65536+rg -> design units
  const add = (lg, rg, v) => { const k = lg * 65536 + rg; raw.set(k, (raw.get(k) ?? 0) + v); };

  const gpos = font.layout('GPOS');
  const kernLookups = new Set();
  if (gpos) for (const f of gpos.features()) if (f.tag === 'kern') for (const i of f.lookupIndices) kernLookups.add(i);

  if (mode === 'sum' || kernLookups.size === 0) {
    for (const pairs of font.kernSubtables()) {
      for (const [k, v] of pairs) {
        const lg = Math.floor(k / 65536), rg = k % 65536;
        if (glyphToCps.has(lg) && glyphToCps.has(rg)) add(lg, rg, v);
      }
    }
  }
  for (const li of kernLookups) {
    const lookup = gpos.lookup(li);
    const matched = mode === 'first-match' ? new Set() : null;
    for (const st of lookup.subtables) {
      if (st.type === 2) gpos.pairPos(st.off, glyphToCps, add, matched);
    }
  }
  return { upem: font.unitsPerEm(), raw, glyphToCps };
}

/** The per-size half: scale, drop zeros, expand aliases, drop SMP. Map<lcp*0x110000+rcp, int8>. */
export function scaleKerning(prep, ppem) {
  const scale = ppem / prep.upem;
  const out = new Map();
  for (const [k, du] of prep.raw) {
    const adjust = fp4FromDesignUnits(du, scale);
    if (adjust === 0) continue;
    const lcps = prep.glyphToCps.get(Math.floor(k / 65536));
    const rcps = prep.glyphToCps.get(k % 65536);
    for (const l of lcps) for (const r of rcps) {
      if (l <= 0xFFFF && r <= 0xFFFF) out.set(l * 0x110000 + r, adjust);
    }
  }
  return out;
}

/**
 * derive_kern_classes(): identical rows/columns share a class. A side needing more than 255 classes
 * (one byte each) first has its closest classes merged (kern-reduce.mjs, as kern_reduce.py does);
 * `reduced` then reports it, else it is null.
 */
export function deriveKernClasses(kernMapIn) {
  const empty = { left: [], right: [], matrix: new Int8Array(0), leftCount: 0, rightCount: 0, dropped: false, reduced: null };
  if (kernMapIn.size === 0) return empty;
  const { map: kernMap, info: reduced } = reduceKernClasses(kernMapIn);
  const lset = new Set(), rset = new Set();
  for (const k of kernMap.keys()) { lset.add(Math.floor(k / 0x110000)); rset.add(k % 0x110000); }
  const L = [...lset].sort((a, b) => a - b), R = [...rset].sort((a, b) => a - b);
  const li = new Map(L.map((c, i) => [c, i])), ri = new Map(R.map((c, i) => [c, i]));
  const grid = new Int8Array(L.length * R.length);
  for (const [k, v] of kernMap) grid[li.get(Math.floor(k / 0x110000)) * R.length + ri.get(k % 0x110000)] = v;

  const leftClass = new Map(), rowClass = new Map();
  for (let i = 0; i < L.length; i++) {
    const key = grid.subarray(i * R.length, (i + 1) * R.length).join(',');
    if (!rowClass.has(key)) rowClass.set(key, rowClass.size + 1);
    leftClass.set(L[i], rowClass.get(key));
  }
  const rightClass = new Map(), colClass = new Map();
  const col = new Int8Array(L.length);
  for (let j = 0; j < R.length; j++) {
    for (let i = 0; i < L.length; i++) col[i] = grid[i * R.length + j];
    const key = col.join(',');
    if (!colClass.has(key)) colClass.set(key, colClass.size + 1);
    rightClass.set(R[j], colClass.get(key));
  }
  const leftCount = rowClass.size, rightCount = colClass.size;
  const matrix = new Int8Array(leftCount * rightCount);
  for (const [k, v] of kernMap) {
    matrix[(leftClass.get(Math.floor(k / 0x110000)) - 1) * rightCount + rightClass.get(k % 0x110000) - 1] = v;
  }
  return {
    left: L.map((c) => [c, leftClass.get(c)]),
    right: R.map((c) => [c, rightClass.get(c)]),
    matrix, leftCount, rightCount, dropped: false, reduced,
  };
}

// --- ligatures ------------------------------------------------------------

export const STANDARD_LIGATURE_MAP = new Map([
  ['102,102', 0xFB00], ['102,105', 0xFB01], ['102,108', 0xFB02],
  ['102,102,105', 0xFB03], ['102,102,108', 0xFB04], ['383,116', 0xFB05], ['115,116', 0xFB06],
]);

/**
 * extract_ligatures_fonttools(): GSUB 'liga'+'rlig' LigatureSubst (type 4, also
 * inside type-7 extensions), lookups visited in CPython set order, glyph->cp from
 * the whole best cmap (last codepoint wins), STANDARD_LIGATURE_MAP fallback,
 * filtered to `codepoints`, 3+ char ligatures chained through their prefix.
 * Returns [[packedPair, ligCp], ...] sorted by packedPair (not yet truncated).
 */
export function extractLigatures(font, codepoints) {
  const cmap = font.bestCmap();
  const glyphToCp = new Map();
  for (const [cp, gid] of cmap) glyphToCp.set(gid, cp);
  const raw = new Map(); // seq key -> { seq, lig }
  const gsub = font.layout('GSUB');
  if (gsub) {
    const indices = new PyIntSet();
    for (const f of gsub.features()) if (f.tag === 'liga' || f.tag === 'rlig') indices.update(f.lookupIndices);
    for (const li of indices) {
      for (const st of gsub.lookup(li).subtables) {
        if (st.type !== 4) continue;
        for (const [first, ligs] of gsub.ligatureSubst(st.off)) {
          if (!glyphToCp.has(first)) continue;
          const firstCp = glyphToCp.get(first);
          for (const lig of ligs) {
            if (!lig.components.every((g) => glyphToCp.has(g))) continue;
            const seq = [firstCp, ...lig.components.map((g) => glyphToCp.get(g))];
            const key = seq.join(',');
            let ligCp;
            if (glyphToCp.has(lig.ligGlyph)) ligCp = glyphToCp.get(lig.ligGlyph);
            else if (STANDARD_LIGATURE_MAP.has(key)) ligCp = STANDARD_LIGATURE_MAP.get(key);
            else continue; // the script warns and drops it
            raw.set(key, { seq, lig: ligCp });
          }
        }
      }
    }
  }
  const cps = codepoints instanceof Set ? codepoints : new Set(codepoints);
  const filtered = new Map();
  for (const [key, { seq, lig }] of raw) {
    if (!cps.has(lig) || lig > 0xFFFF) continue;
    if (seq.some((c) => c > 0xFFFF)) continue;
    if (seq.every((c) => cps.has(c))) filtered.set(key, { seq, lig });
  }
  const pairs = [];
  for (const { seq, lig } of filtered.values()) if (seq.length === 2) pairs.push([seq[0] * 65536 + seq[1], lig]);
  for (const { seq, lig } of filtered.values()) {
    if (seq.length < 3) continue;
    const prefix = filtered.get(seq.slice(0, -1).join(','));
    if (prefix) pairs.push([prefix.lig * 65536 + seq[seq.length - 1], lig]);
  }
  pairs.sort((a, b) => a[0] - b[0]); // stable, like list.sort(key=...)
  return pairs;
}

// --- synthetic bold -------------------------------------------------------

function unpack2bit(bitmap, w, h) {
  const px = new Uint8Array(w * h);
  for (let i = 0; i < px.length; i++) px[i] = (bitmap[i >> 2] >> ((3 - (i & 3)) << 1)) & 3;
  return px;
}

function repack2bit(px) {
  const out = new Uint8Array((px.length + 3) >> 2);
  for (let i = 0; i < px.length; i++) out[i >> 2] |= (px[i] & 3) << ((3 - (i & 3)) << 1);
  return out;
}

export function styleUsesSyntheticBold(base, target) {
  for (const cp of [0x41, 0x61, 0x67, 0x30]) {
    const a = base.glyphs.find((g) => g.codePoint === cp);
    const b = target.glyphs.find((g) => g.codePoint === cp);
    if (!a || !b) return false;
    if (a.width !== b.width || a.height !== b.height) return false;
    const pa = unpack2bit(a.bitmap, a.width, a.height), pb = unpack2bit(b.bitmap, b.width, b.height);
    for (let i = 0; i < pa.length; i++) if (pa[i] !== pb[i]) return false;
  }
  return true;
}

/** apply_synthetic_bold(): 3x3 max-dilation of every glyph's 2-bit pixels, in place. */
export function applySyntheticBold(sd) {
  for (const g of sd.glyphs) {
    if (g.width === 0 || g.height === 0) continue;
    const w = g.width, h = g.height;
    const px = unpack2bit(g.bitmap, w, h);
    const out = new Uint8Array(px.length);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let m = 0;
      for (let dy = -1; dy <= 1; dy++) {
        const ny = y + dy;
        if (ny < 0 || ny >= h) continue;
        for (let dx = -1; dx <= 1; dx++) {
          const nx = x + dx;
          if (nx < 0 || nx >= w) continue;
          if (px[ny * w + nx] > m) m = px[ny * w + nx];
        }
      }
      out[y * w + x] = m;
    }
    g.bitmap = repack2bit(out); // same length; offsets unchanged
  }
}

// --- binary layout --------------------------------------------------------

const LIMITS = { B: [0, 0xFF], b: [-128, 127], H: [0, 0xFFFF], h: [-32768, 32767], I: [0, 0xFFFFFFFF] };

/** A little-endian struct.pack with Python's range errors. */
class Packer {
  constructor(size) { this.buf = new Uint8Array(size); this.dv = new DataView(this.buf.buffer); this.pos = 0; }
  put(fmt, v, what) {
    const [lo, hi] = LIMITS[fmt];
    if (!Number.isInteger(v) || v < lo || v > hi) {
      throw new RangeError(`struct.error: ${what} = ${v} does not fit '${fmt}' (${lo}..${hi})`);
    }
    switch (fmt) {
      case 'B': this.dv.setUint8(this.pos, v); this.pos += 1; break;
      case 'b': this.dv.setInt8(this.pos, v); this.pos += 1; break;
      case 'H': this.dv.setUint16(this.pos, v, true); this.pos += 2; break;
      case 'h': this.dv.setInt16(this.pos, v, true); this.pos += 2; break;
      case 'I': this.dv.setUint32(this.pos, v, true); this.pos += 4; break;
    }
    return this;
  }
  pad(n) { this.pos += n; return this; }
  bytes(u8) { this.buf.set(u8, this.pos); this.pos += u8.length; return this; }
}

/** pack_style_sections(): intervals, glyphs, kern left, kern right, kern matrix, ligatures, bitmaps. */
export function packStyleSections(sd) {
  const iv = new Packer(sd.intervals.length * 12);
  let offset = 0;
  for (const [s, e] of sd.intervals) {
    iv.put('I', s, 'interval start').put('I', e, 'interval end').put('I', offset, 'interval offset');
    offset += e - s + 1;
  }
  const gl = new Packer(sd.glyphs.length * 16);
  let bitmapSize = 0;
  for (const g of sd.glyphs) {
    const cp = `glyph U+${g.codePoint.toString(16).toUpperCase()}`;
    gl.put('B', g.width, `${cp} width`).put('B', g.height, `${cp} height`).put('H', g.advanceX, `${cp} advanceX`)
      .put('h', g.left, `${cp} left`).put('h', g.top, `${cp} top`).put('H', g.dataLength, `${cp} dataLength`)
      .pad(2).put('I', g.dataOffset, `${cp} dataOffset`);
    bitmapSize += g.bitmap.length;
  }
  const kl = new Packer(sd.kern.left.length * 3);
  for (const [cp, cls] of sd.kern.left) kl.put('H', cp, 'kern left cp').put('B', cls, 'kern left class');
  const kr = new Packer(sd.kern.right.length * 3);
  for (const [cp, cls] of sd.kern.right) kr.put('H', cp, 'kern right cp').put('B', cls, 'kern right class');
  const km = new Uint8Array(sd.kern.matrix.buffer, sd.kern.matrix.byteOffset, sd.kern.matrix.length);
  const lg = new Packer(sd.ligatures.length * 8);
  for (const [pair, lig] of sd.ligatures) lg.put('I', pair, 'ligature pair').put('I', lig, 'ligature cp');
  const bm = new Uint8Array(bitmapSize);
  let p = 0;
  for (const g of sd.glyphs) { bm.set(g.bitmap, p); p += g.bitmap.length; }
  if (bitmapSize !== sd.totalBitmapSize) throw new Error('bitmap size mismatch');
  return [iv.buf, gl.buf, kl.buf, kr.buf, km, lg.buf, bm];
}

/** generate_cpfont_multistyle()'s file assembly for one size. `styles`: Map<styleId, styleData>. */
export function assembleCpfont(styles, size) {
  const HEADER_SIZE = 32, TOC_SIZE = 32;
  const ids = [...styles.keys()].sort((a, b) => a - b);
  const sections = new Map(ids.map((id) => [id, packStyleSections(styles.get(id))]));
  let cur = HEADER_SIZE + ids.length * TOC_SIZE;
  const offsets = new Map();
  for (const id of ids) {
    offsets.set(id, cur);
    cur += sections.get(id).reduce((n, s) => n + s.length, 0);
  }
  const out = new Packer(cur);
  out.bytes(new TextEncoder().encode('CPFONT\0\0')).put('H', 4, 'version').put('H', 1, 'flags')
     .put('B', ids.length, 'styleCount').pad(19);
  for (const id of ids) {
    const sd = styles.get(id);
    if (sd.advanceY > 255) {
      throw new RangeError(`advanceY (${sd.advanceY}) exceeds uint8 range for style ${id} size ${size}. ` +
                           'This likely means the font size is too large for this format.');
    }
    out.put('B', id, 'styleId').pad(3)
       .put('I', sd.intervals.length, 'intervalCount').put('I', sd.glyphs.length, 'glyphCount')
       .put('B', sd.advanceY, 'advanceY').put('h', sd.ascender, 'ascender').put('h', sd.descender, 'descender')
       .put('H', sd.kern.left.length, 'kern left entries').put('H', sd.kern.right.length, 'kern right entries')
       .put('B', sd.kern.leftCount, 'kern left classes').put('B', sd.kern.rightCount, 'kern right classes')
       .put('B', sd.ligatures.length, 'ligature count').put('I', offsets.get(id), 'style data offset').pad(4);
  }
  for (const id of ids) for (const s of sections.get(id)) out.bytes(s);
  return out.buf;
}

// --- family conversion ----------------------------------------------------

/** The script's default family name: lowest style's file name minus extension and one style suffix. */
export function familyNameFromFileName(fileName) {
  let base = fileName.replace(/^.*[\\/]/, '').replace(/\.[^.]*$/, '');
  for (const suffix of ['-Regular', '-Bold', '-Italic', '-BoldItalic', '-regular', '-bold', '-italic', '-bolditalic']) {
    if (base.endsWith(suffix)) { base = base.slice(0, -suffix.length); break; }
  }
  return base;
}

/**
 * Convert up to four styles at several sizes into .cpfont files.
 * @param {import('./cpfont-raster.mjs').FreeTypeWasm} ft
 * @param {object} o
 * @param {{regular?:Uint8Array,bold?:Uint8Array,italic?:Uint8Array,bolditalic?:Uint8Array}} o.fonts
 * @param {number[]|string} o.sizes            e.g. [10,12,14,16,18] or "10,12,14,16,18"
 * @param {string} o.intervals                  preset list as in sd-fonts.yaml, e.g. "builtin,latin-cyrillic"
 * @param {string} [o.name]                     family name for file names (build-sd-fonts.py passes --name)
 * @param {{regular?:string,...}} [o.fileNames] used for the default name and error messages
 * @param {boolean} [o.syntheticBold=false]     --synthetic-bold
 * @param {'sum'|'first-match'} [o.kerning='sum']  'first-match' = the OpenType-correct fix (not used yet)
 * @param {(p:{size:number,style:string,done:number,total:number})=>void} [o.onProgress]
 * @returns {{fileName:string,size:number,bytes:Uint8Array,styles:object[]}[]}
 */
export function convertFamily(ft, o) {
  const styleIds = Object.keys(o.fonts ?? {}).filter((k) => o.fonts[k]).map((k) => {
    if (!(k in STYLE_IDS)) throw new Error(`unknown style '${k}' (use regular, bold, italic, bolditalic)`);
    return STYLE_IDS[k];
  }).sort((a, b) => a - b);
  if (!styleIds.length) throw new Error('no font given');
  const sizes = typeof o.sizes === 'string' ? o.sizes.split(',').map((s) => parseInt(s.trim(), 10)) : [...o.sizes];
  if (!sizes.length || sizes.some((s) => !Number.isInteger(s) || s <= 0)) throw new Error(`bad size list ${o.sizes}`);
  const intervals = Array.isArray(o.intervals) ? o.intervals : resolveIntervals(o.intervals);
  const kerningMode = o.kerning ?? 'sum';
  if (kerningMode !== 'sum' && kerningMode !== 'first-match') throw new Error(`kerning must be 'sum' or 'first-match'`);
  const fileNames = o.fileNames ?? {};
  let name = o.name;
  if (!name) {
    const ref = fileNames[STYLE_LABELS[styleIds[0]]];
    if (!ref) throw new Error('name is required when no file name is given');
    name = familyNameFromFileName(ref);
  }

  // Per style, size-independent: parsed tables, refusal checks, kerning sums, ligatures.
  const perStyle = new Map();
  for (const id of styleIds) {
    const label = fileNames[STYLE_LABELS[id]] ?? STYLE_LABELS[id];
    const bytes = o.fonts[STYLE_LABELS[id]];
    const font = new SfntFont(bytes, label);
    if (font.isVariable) {
      throw new FontError(`${label}: variable fonts are not supported (it has an 'fvar' table). ` +
                          'Use the static .ttf/.otf files of the family, or make a static instance first ' +
                          '(fontTools varLib.instancer, as build-sd-fonts.py does).');
    }
    perStyle.set(id, { font, bytes, label, kern: null, ligatures: null });
  }

  const total = sizes.length * styleIds.length;
  let done = 0;
  const results = [];
  for (const size of sizes) {
    const ppem = size * 150.0 / 72.0;
    const styles = new Map();
    for (const id of styleIds) {
      const st = perStyle.get(id);
      const sd = rasterizeStyle(ft, st.bytes, size, intervals, { dpi: CONVERTER_DPI });
      const allCps = new Set(sd.glyphs.map((g) => g.codePoint));
      if (!st.kern) st.kern = prepareKerning(st.font, allCps, kerningMode);
      if (!st.ligatures) st.ligatures = extractLigatures(st.font, allCps);
      sd.kern = deriveKernClasses(scaleKerning(st.kern, ppem));
      sd.ligatures = st.ligatures.slice(0, 255);
      sd.ligaturesTruncated = st.ligatures.length > 255;
      styles.set(id, sd);
      o.onProgress?.({ size, style: STYLE_LABELS[id], done: ++done, total });
    }
    if (o.syntheticBold) {
      const reg = styles.get(0), ita = styles.get(2);
      if (reg && styles.has(1) && styleUsesSyntheticBold(reg, styles.get(1))) applySyntheticBold(styles.get(1));
      if (ita && styles.has(3)) {
        if (styleUsesSyntheticBold(ita, styles.get(3))) applySyntheticBold(styles.get(3));
      } else if (reg && styles.has(3) && styleUsesSyntheticBold(reg, styles.get(3))) {
        applySyntheticBold(styles.get(3));
      }
    }
    const bytes = assembleCpfont(styles, size);
    results.push({
      fileName: `${name}_${size}.cpfont`,
      size,
      bytes,
      styles: [...styles].map(([id, sd]) => ({
        style: STYLE_LABELS[id], glyphs: sd.glyphs.length, intervals: sd.intervals.length,
        bitmapBytes: sd.totalBitmapSize, advanceY: sd.advanceY, ascender: sd.ascender, descender: sd.descender,
        kernLeftClasses: sd.kern.leftCount, kernRightClasses: sd.kern.rightCount, kerningDropped: sd.kern.dropped, kerningReduced: sd.kern.reduced,
        ligatures: sd.ligatures.length, ligaturesTruncated: sd.ligaturesTruncated,
      })),
    });
  }
  return results;
}
