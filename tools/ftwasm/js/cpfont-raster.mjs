// cpfont-raster.mjs -- browser/Node port of the glyph pipeline in
// lib/EpdFont/scripts/fontconvert_sdcard.py (rasterize_font_style()).
//
// Plain ES module with no Node or DOM dependencies, so it runs unchanged in a
// module Web Worker. The FreeType build comes from build.sh (ftwasm.mjs/.wasm).
//
//   import createFtWasm from './ftwasm.mjs';
//   const ft = await FreeTypeWasm.create(createFtWasm);
//   const sd = rasterizeStyle(ft, fontBytes, 14, resolveIntervals('builtin,latin-cyrillic'));
//
// Mirrors (Python name -> here):
//   INTERVAL_PRESETS / resolve_intervals / is_default_ignorable  -> same names (camelCase)
//   rasterize_font_style()  -> rasterizeStyle()  (glyphs, bitmaps, advanceY/ascender/descender;
//                                                 kerning and ligatures are not ported yet)

export const FT_LOAD_DEFAULT = 0x0;
export const FT_LOAD_NO_HINTING = 0x2;
export const FT_LOAD_RENDER = 0x4;
export const FT_LOAD_FORCE_AUTOHINT = 0x20;
export const FT_LOAD_NO_BITMAP = 0x8;

// fontconvert_sdcard.py: load_flags = FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT, 150 DPI.
export const CONVERTER_LOAD_FLAGS = FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT;
export const CONVERTER_DPI = 150;

export const INTERVAL_PRESETS = {
  ascii: [[0x0020, 0x007E]],
  latin1: [[0x0080, 0x00FF]],
  'latin-ext': [[0x0020, 0x007E], [0x0080, 0x00FF], [0x0100, 0x024F], [0x1E00, 0x1EFF], [0x2000, 0x206F]],
  'latin-cyrillic': [[0x0020, 0x007E], [0x0080, 0x00FF], [0x0100, 0x024F], [0x1E00, 0x1EFF], [0x2000, 0x206F],
                     [0x0400, 0x04FF], [0x0500, 0x052F], [0x1C80, 0x1C8F], [0x2DE0, 0x2DFF], [0xA640, 0xA69F]],
  ipa: [[0x0250, 0x02AF], [0x02B0, 0x02FF], [0x03B2, 0x03B2], [0x03B8, 0x03B8], [0x03C7, 0x03C7]],
  greek: [[0x0370, 0x03FF], [0x1F00, 0x1FFF]],
  'greek-letters': [[0x0370, 0x03FF]],
  'musical-symbols': [[0x2660, 0x266F], [0x1D100, 0x1D1FF]],
  cyrillic: [[0x0400, 0x04FF], [0x0500, 0x052F], [0x1C80, 0x1C8F], [0x2DE0, 0x2DFF], [0xA640, 0xA69F]],
  georgian: [[0x10A0, 0x10FF], [0x2D00, 0x2D2F]],
  armenian: [[0x0530, 0x058F]],
  ethiopic: [[0x1200, 0x137F], [0x1380, 0x139F], [0x2D80, 0x2DDF]],
  vietnamese: [[0x01A0, 0x01B0], [0x1EA0, 0x1EF9]],
  punctuation: [[0x2000, 0x206F]],
  cjk: [[0x3000, 0x303F], [0x3040, 0x309F], [0x30A0, 0x30FF], [0x4E00, 0x9FFF], [0xF900, 0xFAFF], [0xFF00, 0xFFEF]],
  hangul: [[0xAC00, 0xD7AF], [0x1100, 0x11FF], [0x3130, 0x318F]],
  builtin: [[0x0000, 0x007F], [0x0080, 0x00FF], [0x0100, 0x017F], [0x01A0, 0x01A1], [0x01AF, 0x01B0],
            [0x01C4, 0x021F], [0x0300, 0x036F], [0x0400, 0x04FF], [0x1EA0, 0x1EF9], [0x2000, 0x206F],
            [0x20A0, 0x20CF], [0x2070, 0x209F], [0x2190, 0x21FF], [0x2200, 0x22FF], [0xFB00, 0xFB06]],
  reading: [[0x0020, 0x02FF], [0x0300, 0x036F], [0x0370, 0x03FF], [0x0400, 0x04FF], [0x1E00, 0x1EFF],
            [0x2000, 0x206F], [0x2070, 0x209F], [0x20A0, 0x20CF], [0x2150, 0x218F], [0x2190, 0x21FF],
            [0x2200, 0x22FF], [0x2500, 0x257F], [0x25A0, 0x25FF], [0x2600, 0x26FF], [0x2700, 0x27BF],
            [0x2900, 0x29FF], [0x2E00, 0x2E7F], [0x3000, 0x303F], [0xFB00, 0xFB06]],
};

// Kept identical to DEFAULT_IGNORABLE_RANGES in fontconvert_sdcard.py / fontconvert.py.
export const DEFAULT_IGNORABLE_RANGES = [
  [0x034F, 0x034F], [0x061C, 0x061C], [0x115F, 0x1160], [0x17B4, 0x17B5],
  [0x180B, 0x180F], [0x200B, 0x200F], [0x202A, 0x202E], [0x2060, 0x2064],
  [0x2065, 0x2065], [0x206A, 0x206F], [0x3164, 0x3164], [0xFE00, 0xFE0F],
  [0xFEFF, 0xFEFF], [0xFFA0, 0xFFA0], [0xFFF0, 0xFFF8],
];

export function isDefaultIgnorable(cp) {
  for (const [lo, hi] of DEFAULT_IGNORABLE_RANGES) {
    if (lo <= cp && cp <= hi) return true;
    if (cp < lo) break;
  }
  return false;
}

/** Comma-separated preset names -> merged, sorted interval list (+ U+FFFD), like resolve_intervals(). */
export function resolveIntervals(presetStr) {
  const all = [];
  for (const raw of presetStr.split(',')) {
    const name = raw.trim().toLowerCase();
    const ranges = INTERVAL_PRESETS[name];
    if (!ranges) throw new Error(`unknown interval preset '${name}'`);
    for (const r of ranges) all.push([r[0], r[1]]);
  }
  all.push([0xFFFD, 0xFFFD]);
  all.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const merged = [];
  for (const [s, e] of all) {
    const last = merged[merged.length - 1];
    if (last && s <= last[1] + 1) last[1] = Math.max(last[1], e);
    else merged.push([s, e]);
  }
  return merged;
}

/** fp4_from_ft16_16: FreeType 16.16 -> 12.4 fixed point, rounded. */
export const fp4FromFt16_16 = (v) => (v + (1 << 11)) >> 12;

/** Thin wrapper over the ftw_* exports of ftwasm.c. One instance per worker. */
export class FreeTypeWasm {
  static async create(createFtWasm, moduleArgs = {}) {
    const m = await createFtWasm(moduleArgs);
    const err = m._ftw_init();
    if (err) throw new Error(`FT_Init_FreeType failed: ${err}`);
    return new FreeTypeWasm(m);
  }

  constructor(m) {
    this.m = m;
    this.scratch = m._malloc(64 * 4); // int32 out-array for the *_info calls
  }

  ints(n) {
    const base = this.scratch >> 2;
    return this.m.HEAP32.subarray(base, base + n);
  }

  version() {
    this.m._ftw_version(this.scratch);
    const [major, minor, patch, harfbuzz] = this.ints(4);
    return { major, minor, patch, harfbuzz: !!harfbuzz, string: `${major}.${minor}.${patch}` };
  }

  /** Copies the bytes into wasm memory; they are freed by Face.done(). */
  openFace(bytes, faceIndex = 0) {
    const m = this.m;
    const ptr = m._malloc(bytes.length);
    m.HEAPU8.set(bytes, ptr);
    const err = m._ftw_new_memory_face(ptr, bytes.length, faceIndex, this.scratch);
    if (err) {
      m._free(ptr);
      throw new Error(`FT_New_Memory_Face failed: FreeType error 0x${err.toString(16)}`);
    }
    return new Face(this, this.ints(1)[0], ptr);
  }
}

export class Face {
  constructor(ft, handle, dataPtr) {
    this.ft = ft;
    this.m = ft.m;
    this.h = handle;
    this.dataPtr = dataPtr;
  }

  check(err, what) {
    if (err) throw new Error(`${what} failed: FreeType error 0x${err.toString(16)}`);
  }

  setCharSize(charWidth, charHeight, hres, vres) {
    this.check(this.m._ftw_set_char_size(this.h, charWidth, charHeight, hres, vres), 'FT_Set_Char_Size');
  }

  setPixelSizes(w, h) { this.check(this.m._ftw_set_pixel_sizes(this.h, w, h), 'FT_Set_Pixel_Sizes'); }

  getCharIndex(cp) { return this.m._ftw_get_char_index(this.h, cp) >>> 0; }

  loadGlyph(gid, flags) { this.check(this.m._ftw_load_glyph(this.h, gid, flags), `FT_Load_Glyph(${gid})`); }

  renderGlyph(mode = 0) { this.check(this.m._ftw_render_glyph(this.h, mode), 'FT_Render_Glyph'); }

  /** face->glyph after a load; `buffer` is a wasm pointer valid until the next load. */
  glyphInfo() {
    this.m._ftw_glyph_info(this.h, this.ft.scratch);
    const v = this.ft.ints(21);
    return {
      width: v[0], rows: v[1], pitch: v[2], left: v[3], top: v[4], buffer: v[5] >>> 0,
      pixelMode: v[6], numGrays: v[7], linearHoriAdvance: v[8], linearVertAdvance: v[9],
      advanceX: v[10], advanceY: v[11], lsbDelta: v[12], rsbDelta: v[13],
      metrics: { width: v[14], height: v[15], horiBearingX: v[16], horiBearingY: v[17], horiAdvance: v[18] },
      glyphIndex: v[19] >>> 0, format: v[20] >>> 0,
    };
  }

  /** face->size->metrics (26.6 / 16.16 as in FreeType). */
  sizeMetrics() {
    this.m._ftw_size_metrics(this.h, this.ft.scratch);
    const v = this.ft.ints(8);
    return { xPpem: v[0], yPpem: v[1], xScale: v[2], yScale: v[3], ascender: v[4], descender: v[5], height: v[6], maxAdvance: v[7] };
  }

  faceInfo() {
    this.m._ftw_face_info(this.h, this.ft.scratch);
    const v = Array.from(this.ft.ints(17));
    return {
      unitsPerEM: v[0], ascender: v[1], descender: v[2], height: v[3], numGlyphs: v[4],
      faceFlags: v[5], styleFlags: v[6], numFaces: v[7],
      familyName: v[8] ? this.m.UTF8ToString(v[8] >>> 0) : '',
      styleName: v[9] ? this.m.UTF8ToString(v[9] >>> 0) : '',
      maxAdvanceWidth: v[10], underlinePosition: v[11], underlineThickness: v[12],
      bbox: { xMin: v[13], yMin: v[14], xMax: v[15], yMax: v[16] },
    };
  }

  done() {
    if (!this.h) return;
    this.m._ftw_done_face(this.h);
    this.m._free(this.dataPtr);
    this.h = 0;
  }
}

/**
 * The 2-bit packing of rasterize_font_style(): the 8-bit coverage goes through
 * a 4-bit step (v >> 4) and the 12/8/4 thresholds, which is exactly v >> 6;
 * pixels are packed row-major and continuous across rows (no per-row padding),
 * 4 per byte, first pixel in the top two bits, last byte zero-padded.
 */
export function pack2bit(heapU8, buffer, width, rows, pitch) {
  const out = new Uint8Array((width * rows + 3) >> 2);
  const absPitch = Math.abs(pitch);
  let idx = 0;
  for (let y = 0; y < rows; y++) {
    const row = buffer + (pitch >= 0 ? y : rows - 1 - y) * absPitch;
    for (let x = 0; x < width; x++, idx++) {
      out[idx >> 2] |= (heapU8[row + x] >> 6) << ((3 - (idx & 3)) << 1);
    }
  }
  return out;
}

const emptyGlyph = (codePoint, dataOffset) => ({
  codePoint, width: 0, height: 0, advanceX: 0, left: 0, top: 0, dataLength: 0, dataOffset, bitmap: new Uint8Array(0),
});

/**
 * One style at one size: the glyph half of fontconvert_sdcard.py rasterize_font_style().
 * Returns { intervals, glyphs, bitmap, totalBitmapSize, advanceY, ascender, descender, freetype }.
 * glyphs[i] = { codePoint, width, height, advanceX (12.4), left, top, dataLength, dataOffset, bitmap }.
 */
export function rasterizeStyle(ft, fontBytes, size, intervals, opts = {}) {
  const loadFlags = opts.loadFlags ?? CONVERTER_LOAD_FLAGS;
  const dpi = opts.dpi ?? CONVERTER_DPI;
  const face = ft.openFace(fontBytes, opts.faceIndex ?? 0);
  try {
    // Size first, before any glyph load (same order as the Python script).
    face.setCharSize(size << 6, size << 6, dpi, dpi);

    // Validate intervals: drop codepoints the font has no cmap entry for,
    // but keep default-ignorables (they are emitted empty).
    // The Python script also renders each glyph here; rendering has no side
    // effect on later loads, so only the cmap is consulted.
    const validated = [];
    for (const [iStart, iEnd] of intervals) {
      let start = iStart;
      for (let cp = iStart; cp <= iEnd; cp++) {
        if (isDefaultIgnorable(cp)) continue;
        if (face.getCharIndex(cp) === 0) {
          if (start < cp) validated.push([start, cp - 1]);
          start = cp + 1;
        }
      }
      if (start <= iEnd) validated.push([start, iEnd]);
    }

    const glyphs = [];
    let total = 0;
    for (const [iStart, iEnd] of validated) {
      for (let cp = iStart; cp <= iEnd; cp++) {
        if (isDefaultIgnorable(cp)) { glyphs.push(emptyGlyph(cp, total)); continue; }
        const gid = face.getCharIndex(cp);
        if (gid === 0) { glyphs.push(emptyGlyph(cp, total)); continue; }
        face.loadGlyph(gid, loadFlags);
        const g = face.glyphInfo();
        const packed = pack2bit(ft.m.HEAPU8, g.buffer, g.width, g.rows, g.pitch);
        glyphs.push({
          codePoint: cp,
          width: g.width,
          height: g.rows,
          advanceX: fp4FromFt16_16(g.linearHoriAdvance),
          left: g.left,
          top: g.top,
          dataLength: packed.length,
          dataOffset: total,
          bitmap: packed,
        });
        total += packed.length;
      }
    }

    // Metrics: the script loads '|' first (heuristic shared with fontconvert.py),
    // then reads the active size metrics.
    const pipe = face.getCharIndex(0x7C);
    if (pipe) face.loadGlyph(pipe, loadFlags);
    const sm = face.sizeMetrics();

    const bitmap = new Uint8Array(total);
    for (const g of glyphs) {
      bitmap.set(g.bitmap, g.dataOffset);
      g.bitmap = bitmap.subarray(g.dataOffset, g.dataOffset + g.dataLength);
    }
    return {
      intervals: validated,
      glyphs,
      bitmap,
      totalBitmapSize: total,
      advanceY: Math.ceil(sm.height / 64),
      ascender: Math.ceil(sm.ascender / 64),
      descender: Math.floor(sm.descender / 64),
      freetype: ft.version(),
    };
  } finally {
    face.done();
  }
}
