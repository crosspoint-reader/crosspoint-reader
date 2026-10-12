// The fonts embedded in an EPUB, as plain TrueType/OpenType files: for the Font Manager's converter,
// so a book's own typeface can become a font family on the card.
//
//   const { fonts, skipped } = await EpubFonts.extract(zip);   // zip: the book, opened with JSZip
//   EpubFonts.byFamily(fonts)   // [{ family, styles: { regular, bold, italic, bolditalic } }]
//   await EpubFonts.read(arrayBuffer)   // one font file (TTF, OTF or WOFF), described the same way
//
// Each font: { path, family, subfamily, weight, italic, bold, style, variable, data } where data is an
// ArrayBuffer holding an sfnt (TrueType or CFF OpenType). skipped lists what could not be used, with
// a reason a person can read.
//
// Obfuscated fonts (META-INF/encryption.xml) are decoded: the IDPF algorithm XORs the first 1040
// bytes with the SHA-1 of the book's unique identifier, the Adobe one the first 1024 bytes with the
// 16 bytes of its urn:uuid. Real encryption (DRM) is refused. WOFF is unpacked; WOFF2 is not.
//
// The pages are served over plain http, which is not a secure context, so crypto.subtle is missing:
// SHA-1 is computed here.
(function () {
  'use strict';

  const IDPF_ALGORITHM = 'http://www.idpf.org/2008/embedding';
  const ADOBE_ALGORITHM = 'http://ns.adobe.com/pdf/enc#RC';
  const FONT_EXTENSIONS = /\.(ttf|otf|woff|woff2)$/i;
  const FONT_MEDIA_TYPES = /font|opentype|sfnt|truetype/i;

  // ---- SHA-1 (FIPS 180-4), for the IDPF key ------------------------------------------------------

  function sha1(bytes) {
    const ml = bytes.length;
    const words = new Uint32Array((((ml + 8) >> 6) + 1) * 16);
    for (let i = 0; i < ml; i++) words[i >> 2] |= bytes[i] << (24 - (i % 4) * 8);
    words[ml >> 2] |= 0x80 << (24 - (ml % 4) * 8);
    words[words.length - 1] = ml * 8;
    let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
    const w = new Uint32Array(80);
    for (let block = 0; block < words.length; block += 16) {
      for (let t = 0; t < 16; t++) w[t] = words[block + t];
      for (let t = 16; t < 80; t++) {
        const x = w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16];
        w[t] = (x << 1) | (x >>> 31);
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4;
      for (let t = 0; t < 80; t++) {
        let f, k;
        if (t < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (t < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        const temp = (((a << 5) | (a >>> 27)) + f + e + k + w[t]) >>> 0;
        e = d; d = c; c = (b << 30) | (b >>> 2); b = a; a = temp;
      }
      h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
    }
    const out = new Uint8Array(20);
    [h0, h1, h2, h3, h4].forEach((h, i) => {
      out[i * 4] = h >>> 24; out[i * 4 + 1] = (h >>> 16) & 0xff; out[i * 4 + 2] = (h >>> 8) & 0xff; out[i * 4 + 3] = h & 0xff;
    });
    return out;
  }

  // ---- The book: its package document, identifier and obfuscation list -------------------------

  const parseXml = (text) => new DOMParser().parseFromString(text, 'application/xml');
  const byLocalName = (doc, name) => Array.from(doc.getElementsByTagNameNS('*', name));

  function resolve(base, href) {
    const parts = (base ? base.split('/').slice(0, -1) : []);
    let decoded = href.split('#')[0];
    try { decoded = decodeURI(decoded); } catch (e) { /* a malformed escape: use the href as written */ }
    for (const seg of decoded.split('/')) {
      if (seg === '..') parts.pop();
      else if (seg && seg !== '.') parts.push(seg);
    }
    return parts.join('/');
  }

  async function readPackage(zip) {
    const container = zip.file('META-INF/container.xml');
    if (!container) return null;
    const rootfile = byLocalName(parseXml(await container.async('string')), 'rootfile')[0];
    const opfPath = rootfile && rootfile.getAttribute('full-path');
    const opfFile = opfPath && zip.file(opfPath);
    if (!opfFile) return null;
    const opf = parseXml(await opfFile.async('string'));
    const pkg = byLocalName(opf, 'package')[0];
    const uidRef = pkg ? pkg.getAttribute('unique-identifier') : null;
    const identifiers = byLocalName(opf, 'identifier');
    const unique = identifiers.find((el) => el.getAttribute('id') === uidRef) || identifiers[0];
    const manifest = byLocalName(opf, 'item').map((item) => ({
      path: resolve(opfPath, item.getAttribute('href') || ''),
      mediaType: item.getAttribute('media-type') || ''
    }));
    return {
      opfPath,
      uniqueIdentifier: unique ? unique.textContent : '',
      uuid: identifiers.map((el) => el.textContent.trim()).find((t) => /^urn:uuid:/i.test(t)) || '',
      manifest
    };
  }

  // { path -> algorithm URI } from META-INF/encryption.xml.
  async function readEncryption(zip) {
    const file = zip.file('META-INF/encryption.xml');
    const out = new Map();
    if (!file) return out;
    const doc = parseXml(await file.async('string'));
    for (const data of byLocalName(doc, 'EncryptedData')) {
      const method = byLocalName(data, 'EncryptionMethod')[0];
      const ref = byLocalName(data, 'CipherReference')[0];
      if (!method || !ref) continue;
      out.set(resolve('', ref.getAttribute('URI') || ''), method.getAttribute('Algorithm') || '');
    }
    return out;
  }

  function idpfKey(uniqueIdentifier) {
    // The identifier with all XML whitespace removed, as UTF-8.
    return sha1(new TextEncoder().encode(uniqueIdentifier.replace(/[ \u0009\u000d\u000a]/g, '')));
  }

  function adobeKey(uuid) {
    const hex = uuid.replace(/^urn:uuid:/i, '').replace(/-/g, '');
    if (!/^[0-9a-f]{32}$/i.test(hex)) return null;
    const key = new Uint8Array(16);
    for (let i = 0; i < 16; i++) key[i] = parseInt(hex.substr(i * 2, 2), 16);
    return key;
  }

  function xorPrefix(buffer, key, length) {
    const bytes = new Uint8Array(buffer.slice(0));
    const n = Math.min(length, bytes.length);
    for (let i = 0; i < n; i++) bytes[i] ^= key[i % key.length];
    return bytes.buffer;
  }

  // ---- Font files: WOFF unpacking and the name, OS/2, head and fvar tables ---------------------

  const tag = (view, offset) => String.fromCharCode(view.getUint8(offset), view.getUint8(offset + 1),
    view.getUint8(offset + 2), view.getUint8(offset + 3));

  async function inflate(bytes) {
    if (typeof DecompressionStream === 'undefined') throw new Error('this browser cannot unpack WOFF fonts');
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  // WOFF 1.0 to a plain sfnt: the same tables, decompressed, behind a rebuilt table directory.
  async function unwoff(buffer) {
    const view = new DataView(buffer);
    const numTables = view.getUint16(12);
    const tables = [];
    for (let i = 0; i < numTables; i++) {
      const o = 44 + i * 20;
      const offset = view.getUint32(o + 4), compLength = view.getUint32(o + 8), origLength = view.getUint32(o + 12);
      const raw = new Uint8Array(buffer, offset, compLength);
      tables.push({
        tag: tag(view, o), checksum: view.getUint32(o + 16),
        data: compLength < origLength ? await inflate(raw) : raw.slice()
      });
    }
    let size = 12 + numTables * 16;
    const offsets = tables.map((t) => { const at = size; size += (t.data.length + 3) & ~3; return at; });
    const out = new Uint8Array(size);
    const outView = new DataView(out.buffer);
    outView.setUint32(0, view.getUint32(4));  // the original sfnt version
    outView.setUint16(4, numTables);
    let pow = 1, log = 0;
    while (pow * 2 <= numTables) { pow *= 2; log++; }
    outView.setUint16(6, pow * 16); outView.setUint16(8, log); outView.setUint16(10, numTables * 16 - pow * 16);
    tables.forEach((t, i) => {
      const o = 12 + i * 16;
      for (let k = 0; k < 4; k++) out[o + k] = t.tag.charCodeAt(k);
      outView.setUint32(o + 4, t.checksum); outView.setUint32(o + 8, offsets[i]); outView.setUint32(o + 12, t.data.length);
      out.set(t.data, offsets[i]);
    });
    return out.buffer;
  }

  function tableDirectory(view) {
    const tables = {};
    const count = view.getUint16(4);
    for (let i = 0; i < count; i++) {
      const o = 12 + i * 16;
      tables[tag(view, o)] = { offset: view.getUint32(o + 8), length: view.getUint32(o + 12) };
    }
    return tables;
  }

  // The best name for nameID: Windows Unicode US English, then any Windows Unicode, then Mac Roman.
  function readName(view, table, nameId) {
    if (!table) return '';
    const count = view.getUint16(table.offset + 2);
    const strings = table.offset + view.getUint16(table.offset + 4);
    let best = null, bestScore = -1;
    for (let i = 0; i < count; i++) {
      const r = table.offset + 6 + i * 12;
      if (view.getUint16(r + 6) !== nameId) continue;
      const platform = view.getUint16(r), encoding = view.getUint16(r + 2), language = view.getUint16(r + 4);
      const score = platform === 3 && (encoding === 1 || encoding === 10) ? (language === 0x409 ? 3 : 2)
        : platform === 0 ? 2 : platform === 1 && encoding === 0 ? 1 : -1;
      if (score > bestScore) { bestScore = score; best = { platform, length: view.getUint16(r + 8), offset: view.getUint16(r + 10) }; }
    }
    if (!best) return '';
    const start = strings + best.offset;
    let text = '';
    if (best.platform === 1) {
      for (let i = 0; i < best.length; i++) text += String.fromCharCode(view.getUint8(start + i));
    } else {
      for (let i = 0; i + 1 < best.length; i += 2) text += String.fromCharCode(view.getUint16(start + i));
    }
    return text.trim();
  }

  function describe(buffer) {
    const view = new DataView(buffer);
    const version = tag(view, 0);
    if (version === 'ttcf') throw new Error('a font collection, which the converter cannot take');
    if (version !== 'OTTO' && version !== 'true' && view.getUint32(0) !== 0x00010000) {
      throw new Error('not a TrueType or OpenType font');
    }
    const tables = tableDirectory(view);
    const family = readName(view, tables.name, 16) || readName(view, tables.name, 1);
    const subfamily = readName(view, tables.name, 17) || readName(view, tables.name, 2) || 'Regular';
    let weight = 400, italic = /italic|oblique/i.test(subfamily), bold = /bold|black|heavy/i.test(subfamily);
    if (tables['OS/2'] && tables['OS/2'].length >= 64) {
      weight = view.getUint16(tables['OS/2'].offset + 4);
      const fsSelection = view.getUint16(tables['OS/2'].offset + 62);
      italic = italic || (fsSelection & 1) !== 0;
      bold = bold || (fsSelection & 0x20) !== 0;
    } else if (tables.head) {
      const macStyle = view.getUint16(tables.head.offset + 44);
      bold = bold || (macStyle & 1) !== 0;
      italic = italic || (macStyle & 2) !== 0;
    }
    bold = bold || weight >= 600;
    return {
      family: family || 'Unnamed font', subfamily, weight, italic, bold,
      style: (bold ? 'bold' : '') + (italic ? 'italic' : '') || 'regular',
      variable: !!tables.fvar
    };
  }

  // ---- Putting it together ------------------------------------------------------------------------

  async function extract(zip) {
    const pkg = await readPackage(zip);
    const encryption = await readEncryption(zip);
    const declared = new Set(pkg ? pkg.manifest.filter((m) => FONT_MEDIA_TYPES.test(m.mediaType)).map((m) => m.path) : []);
    const candidates = Object.keys(zip.files).filter((p) => !zip.files[p].dir && (FONT_EXTENSIONS.test(p) || declared.has(p)));
    const fonts = [], skipped = [];
    for (const path of candidates) {
      try {
        let data = await zip.file(path).async('arraybuffer');
        const algorithm = encryption.get(path);
        if (algorithm === IDPF_ALGORITHM) {
          if (!pkg || !pkg.uniqueIdentifier) throw new Error('obfuscated, and the book has no identifier to undo it');
          data = xorPrefix(data, idpfKey(pkg.uniqueIdentifier), 1040);
        } else if (algorithm === ADOBE_ALGORITHM) {
          const key = pkg && adobeKey(pkg.uuid);
          if (!key) throw new Error('obfuscated, and the book has no urn:uuid identifier to undo it');
          data = xorPrefix(data, key, 1024);
        } else if (algorithm) {
          throw new Error('encrypted (DRM)');
        }
        const signature = tag(new DataView(data), 0);
        if (signature === 'wOF2') throw new Error('a WOFF2 font, which cannot be unpacked here');
        if (signature === 'wOFF') data = await unwoff(data);
        fonts.push(Object.assign({ path, data }, describe(data)));
      } catch (e) {
        skipped.push({ path, reason: e.message });
      }
    }
    return { fonts, skipped };
  }

  // One entry per family, its styles keyed regular / bold / italic / bolditalic; a second font of the
  // same style (a semibold beside the bold, say) goes to `extra`.
  function byFamily(fonts) {
    const families = new Map();
    for (const font of fonts) {
      if (!families.has(font.family)) families.set(font.family, { family: font.family, styles: {}, extra: [] });
      const entry = families.get(font.family);
      if (!entry.styles[font.style]) entry.styles[font.style] = font;
      else entry.extra.push(font);
    }
    return Array.from(families.values());
  }

  // One font file chosen by hand: unpacked if WOFF, described like an extracted one. Throws with a
  // readable reason for anything the converter cannot take.
  async function read(buffer) {
    let data = buffer;
    const signature = tag(new DataView(data), 0);
    if (signature === 'wOF2') throw new Error('a WOFF2 font, which cannot be unpacked here');
    if (signature === 'wOFF') data = await unwoff(data);
    return Object.assign({ data }, describe(data));
  }

  window.EpubFonts = { extract, byFamily, read, sha1 };
})();
