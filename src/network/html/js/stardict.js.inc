// --- StarDict helpers for the Dictionaries page -----------------------------
// What the reader accepts (src/util/DictionaryRegistry.cpp, docs/dictionary.md):
// one dictionary per folder under /dictionaries, holding <stem>.idx (plain, never
// .idx.gz) and <stem>.dict or <stem>.dict.dz, optionally <stem>.syn and
// <stem>.ifo; an .ifo with idxoffsetbits=64 is refused. Everything the page
// installs is brought into that shape in the browser, because the device has
// neither the memory nor the time to unpack archives itself.
//
// Kept apart from the page's DOM code and free of it, so the archive readers
// and the rules can be run outside a browser.

// Longest suffix first, so ".idx.gz" is not read as some ".gz". `unpack` is
// 'always' where the reader cannot use the compressed form at all, 'offer' for
// .dict.dz, which works but costs a 32 KB inflate window per lookup.
const DICT_SUFFIXES = [
  { suffix: '.dict.dz', kind: 'dict', unpack: 'offer' },
  { suffix: '.dict.gz', kind: 'dict', unpack: 'always' },
  { suffix: '.idx.gz', kind: 'idx', unpack: 'always' },
  { suffix: '.syn.gz', kind: 'syn', unpack: 'always' },
  { suffix: '.dict', kind: 'dict', unpack: null },
  { suffix: '.idx', kind: 'idx', unpack: null },
  { suffix: '.syn', kind: 'syn', unpack: null },
  { suffix: '.ifo', kind: 'ifo', unpack: null },
];

// The upload order: a cut-off install then lacks its .dict, which the reader
// treats as "not a dictionary" rather than offering one that fails at lookup.
const UPLOAD_ORDER = ['ifo', 'syn', 'idx', 'dict'];

const textDecoder = new TextDecoder('utf-8');

function formatSize(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + ' GB';
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
  return bytes + ' B';
}

function baseName(path) {
  const cut = path.lastIndexOf('/');
  return cut >= 0 ? path.slice(cut + 1) : path;
}

function dirName(path) {
  const cut = path.lastIndexOf('/');
  return cut >= 0 ? path.slice(0, cut) : '';
}

// Junk an archive made on a Mac carries along: AppleDouble resource forks.
// No regex literal here: build_html.py's fallback minifier (no terser) reads
// "return /..\//" as a division and the "//" as a comment, and cuts the line.
function isJunkPath(path) {
  return path.startsWith('__MACOSX/') || path.includes('/__MACOSX/') || baseName(path).startsWith('._');
}

// { kind, stem, unpack } for a dictionary file name, or null for anything else.
function classifyDictFile(name) {
  const lower = name.toLowerCase();
  for (const s of DICT_SUFFIXES) {
    if (lower.endsWith(s.suffix) && lower.length > s.suffix.length) {
      return { kind: s.kind, stem: name.slice(0, -s.suffix.length), unpack: s.unpack };
    }
  }
  return null;
}

// The .ifo is "key=value" lines under a fixed first line.
function parseIfo(text) {
  const ifo = {};
  for (const line of text.split(/\r?\n/)) {
    const eq = line.indexOf('=');
    if (eq > 0) ifo[line.slice(0, eq).trim()] = line.slice(eq + 1).trim();
  }
  return ifo;
}

// The reader stores the chosen dictionary's folder in a 32-byte setting
// (CrossPointSettings::dictionaryName): a longer name is cut short there and the
// folder is not found again. Folder names are ASCII, so characters are bytes.
const MAX_FOLDER_NAME = 31;
// Dictionary::open() refuses a "<root>/<folder>/<stem>" longer than this.
const MAX_BASE_PATH = 151;

// A folder name the card and the reader both take: one path component, ASCII
// (the device's UI fonts do not cover every script a bookname can be in), and
// nothing FAT trims or the reader skips (a leading dot, trailing dots or
// spaces). Accents are folded rather than dropped.
function sanitizeFolderName(raw) {
  if (!raw) return '';
  let s = String(raw).normalize('NFKD').replace(/[̀-ͯ]/g, '');
  s = s.replace(/[^A-Za-z0-9 _+()-]+/g, ' ').replace(/\s+/g, ' ').trim();
  if (s.length > MAX_FOLDER_NAME) {
    // At a word boundary when that keeps most of the name: the device lists it.
    const cut = s.lastIndexOf(' ', MAX_FOLDER_NAME);
    s = s.slice(0, cut >= 16 ? cut : MAX_FOLDER_NAME).trim();
  }
  return s;
}

// Whether an archive's own file stem can be kept as it is. Otherwise the files
// are renamed after the folder: the reader only needs the four to agree.
function isSafeStem(stem) {
  return stem.length <= 100 && /^[A-Za-z0-9][A-Za-z0-9 _+(),.-]*$/.test(stem) && !/[ .]$/.test(stem);
}

// The archive's name without its extensions, as a fallback folder name.
function archiveStem(fileName) {
  return fileName.replace(/\.(zip|tgz|tbz2?|tar\.gz|tar\.bz2|tar)$/i, '');
}

// The uncompressed size a gzip member records in its last four bytes
// (modulo 4 GB, which no dictionary reaches).
async function gzipOriginalSize(blob) {
  if (blob.size < 18) return 0;
  const tail = new DataView(await blob.slice(blob.size - 4).arrayBuffer());
  return tail.getUint32(0, true);
}

// A dictzip file is gzip with a random-access table in the header's extra
// field (subfield "RA"). The reader needs that table to read a .dict.dz. Returns
// 'ok', 'plain' (a gzip renamed to .dz, with no table) or 'unsupported' (a table
// DictZip::parse() refuses: deflate, version 1 and 1 to 8192 chunks only, which
// caps the table it keeps in memory at 32 KB). Anything but 'ok' must be unpacked.
async function dictzipKind(blob) {
  const h = new Uint8Array(await blob.slice(0, 22).arrayBuffer());
  if (h.length < 22 || h[0] !== 0x1f || h[1] !== 0x8b || (h[3] & 0x04) === 0 || h[12] !== 0x52 || h[13] !== 0x41) {
    return 'plain';
  }
  const le16 = (i) => h[i] | (h[i + 1] << 8);
  const subLen = le16(14);
  const version = le16(16);
  const chunkLen = le16(18);
  const chunkCount = le16(20);
  const readable = h[2] === 8 && version === 1 && chunkLen > 0 && chunkCount > 0 && chunkCount <= 8192 &&
    subLen === 6 + 2 * chunkCount;
  return readable ? 'ok' : 'unsupported';
}

// Streams a blob through the browser's gzip decoder. dictzip is gzip, so this
// covers .dict.dz as well. onProgress gets the compressed bytes consumed.
async function gunzipBlob(blob, onProgress) {
  if (typeof DecompressionStream === 'undefined') {
    throw new Error('This browser cannot unpack .gz/.dz files. Use a current Chrome, Edge, Firefox or Safari.');
  }
  const stream = countingStream(blob, onProgress).pipeThrough(new DecompressionStream('gzip'));
  return await new Response(stream).blob();
}

// The blob's bytes as a stream, reporting (bytes read, total) as they go.
function countingStream(blob, onProgress) {
  let done = 0;
  return blob.stream().pipeThrough(
    new TransformStream({
      transform(chunk, controller) {
        done += chunk.length;
        if (onProgress) onProgress(done, blob.size);
        controller.enqueue(chunk);
      },
    }),
  );
}

// --- tar --------------------------------------------------------------------
// Reads a tar stream chunk by chunk and hands each regular file that keep()
// accepts to onFile(path, blob); everything else streams past unbuffered. Enough
// of the format for dictionary archives: ustar names with their prefix, GNU
// long names (type L), pax path records (type x), base-256 sizes.
class TarReader {
  constructor(keep, onFile) {
    this.keep = keep;
    this.onFile = onFile;
    this.header = new Uint8Array(512);
    this.headerFill = 0;
    this.dataLeft = 0;
    this.padLeft = 0;
    this.parts = null;
    this.entry = null;
    this.nextName = null;
    this.ended = false;
    this.entries = 0;
  }

  push(chunk) {
    let off = 0;
    while (off < chunk.length && !this.ended) {
      if (this.dataLeft > 0) {
        const n = Math.min(this.dataLeft, chunk.length - off);
        if (this.parts) this.parts.push(chunk.slice(off, off + n));
        off += n;
        this.dataLeft -= n;
        if (this.dataLeft === 0) this.endEntry();
      } else if (this.padLeft > 0) {
        const n = Math.min(this.padLeft, chunk.length - off);
        off += n;
        this.padLeft -= n;
      } else {
        const n = Math.min(512 - this.headerFill, chunk.length - off);
        this.header.set(chunk.subarray(off, off + n), this.headerFill);
        this.headerFill += n;
        off += n;
        if (this.headerFill === 512) {
          this.headerFill = 0;
          this.startEntry();
        }
      }
    }
  }

  // Throws when the data stopped inside an entry: a truncated download.
  finish() {
    if (this.dataLeft > 0 || this.headerFill > 0 || this.entry) throw new Error('The archive is cut off.');
    if (this.entries === 0) throw new Error('This is not a tar archive.');
  }

  startEntry() {
    const h = this.header;
    if (h.every((b) => b === 0)) {
      this.ended = true;  // the end-of-archive blocks
      return;
    }
    let sum = 0;
    for (let i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
    if (TarReader.octal(h, 148, 8) !== sum) throw new Error('This is not a tar archive (bad header checksum).');

    let name = TarReader.text(h, 0, 100);
    // POSIX ustar splits long names over a prefix field; the older GNU format
    // uses those bytes for other things and has "ustar  " as its magic.
    if (TarReader.text(h, 257, 6) === 'ustar' && h[262] === 0) {
      const prefix = TarReader.text(h, 345, 155);
      if (prefix) name = prefix + '/' + name;
    }
    if (this.nextName !== null) {
      name = this.nextName;
      this.nextName = null;
    }
    const type = h[156] === 0 ? '0' : String.fromCharCode(h[156]);
    let size;
    if (h[124] & 0x80) {
      size = 0;
      for (let i = 125; i < 136; i++) size = size * 256 + h[i];
    } else {
      size = TarReader.octal(h, 124, 12);
    }
    this.entries++;
    this.entry = { name: name.replace(/^\.\//, ''), type };
    this.dataLeft = size;
    this.padLeft = (512 - (size % 512)) % 512;
    const meta = type === 'L' || type === 'x';
    const file = type === '0' || type === '7';
    this.parts = meta || (file && this.keep(this.entry.name)) ? [] : null;
    if (size === 0) this.endEntry();
  }

  endEntry() {
    const entry = this.entry;
    const parts = this.parts;
    this.entry = null;
    this.parts = null;
    if (!parts) return;
    if (entry.type === 'L') {
      const bytes = TarReader.join(parts);
      this.nextName = TarReader.text(bytes, 0, bytes.length);
    } else if (entry.type === 'x') {
      const path = TarReader.paxPath(TarReader.join(parts));
      if (path) this.nextName = path;
    } else {
      this.onFile(entry.name, new Blob(parts));
    }
  }

  static text(bytes, off, len) {
    let end = off;
    while (end < off + len && bytes[end] !== 0) end++;
    return textDecoder.decode(bytes.subarray(off, end));
  }

  static octal(bytes, off, len) {
    const s = TarReader.text(bytes, off, len).trim();
    return s ? parseInt(s, 8) : 0;
  }

  static join(parts) {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let off = 0;
    for (const p of parts) {
      out.set(p, off);
      off += p.length;
    }
    return out;
  }

  // pax records are "<length> <key>=<value>\n", the length counted in bytes.
  static paxPath(bytes) {
    let pos = 0;
    while (pos < bytes.length) {
      let space = pos;
      while (space < bytes.length && bytes[space] !== 0x20) space++;
      const len = parseInt(textDecoder.decode(bytes.subarray(pos, space)), 10);
      if (!(len > 0)) break;
      const record = textDecoder.decode(bytes.subarray(space + 1, pos + len)).replace(/\n$/, '');
      const eq = record.indexOf('=');
      if (eq > 0 && record.slice(0, eq) === 'path') return record.slice(eq + 1);
      pos += len;
    }
    return null;
  }
}

// --- archives ---------------------------------------------------------------
// Each reader returns [{ path, blob }] for the files keep(path) accepts, and
// reports progress as (bytes done, bytes total) of the archive file.

function yieldToBrowser() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

async function readTarStream(stream, keep) {
  const files = [];
  const tar = new TarReader(keep, (path, blob) => files.push({ path, blob }));
  const reader = stream.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    tar.push(value);
    if (tar.ended) {
      await reader.cancel();
      break;
    }
  }
  tar.finish();
  return files;
}

async function readTar(file, keep, onProgress) {
  return readTarStream(countingStream(file, onProgress), keep);
}

async function readTarGz(file, keep, onProgress) {
  if (typeof DecompressionStream === 'undefined') {
    throw new Error('This browser cannot unpack .tar.gz archives. Use a current Chrome, Edge, Firefox or Safari, or a .zip.');
  }
  return readTarStream(countingStream(file, onProgress).pipeThrough(new DecompressionStream('gzip')), keep);
}

// bzip2 has no native decoder in browsers; Bunzip2 (js/bunzip2.js.inc) decodes
// it a block at a time, and the page yields between blocks to stay responsive.
async function readTarBz2(file, keep, onProgress) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const decoder = new Bunzip2.Decoder(bytes);
  const files = [];
  const tar = new TarReader(keep, (path, blob) => files.push({ path, blob }));
  for (let block = decoder.nextBlock(); block && !tar.ended; block = decoder.nextBlock()) {
    tar.push(block);
    if (onProgress) onProgress(decoder.inputPos(), bytes.length);
    await yieldToBrowser();
  }
  tar.finish();
  return files;
}

async function readZip(file, keep) {
  if (typeof JSZip === 'undefined') throw new Error('The zip reader did not load. Reload the page and try again.');
  // JSZip reads the whole archive either way; handing it the bytes rather than
  // the File spares it FileReader, which not every environment has.
  const zip = await JSZip.loadAsync(await file.arrayBuffer());
  const files = [];
  const entries = [];
  zip.forEach((path, entry) => {
    if (!entry.dir && keep(path)) entries.push(entry);
  });
  for (const entry of entries) files.push({ path: entry.name, blob: await entry.async('blob') });
  return files;
}

// What a picked file is, by its name.
function archiveKind(name) {
  const lower = name.toLowerCase();
  if (lower.endsWith('.zip')) return 'zip';
  if (lower.endsWith('.tar.gz') || lower.endsWith('.tgz')) return 'tar.gz';
  if (lower.endsWith('.tar.bz2') || lower.endsWith('.tbz2') || lower.endsWith('.tbz')) return 'tar.bz2';
  if (lower.endsWith('.tar')) return 'tar';
  if (lower.endsWith('.tar.xz') || lower.endsWith('.txz') || lower.endsWith('.7z') || lower.endsWith('.rar')) {
    return 'unsupported';
  }
  return null;
}

// Only the files a dictionary is made of; an archive's README, licence and
// res/ pictures are never unpacked.
function keepDictFile(path) {
  return !isJunkPath(path) && classifyDictFile(baseName(path)) !== null;
}

// --- finding the dictionaries in a set of files ------------------------------

// Groups files into dictionaries by folder and stem. Returns
// { dicts: [group], stray: [path] } where a group has one source per kind
// (ifo, idx, dict, syn) and `skipped` for duplicates it did not use.
function groupDictionaries(sources) {
  const groups = new Map();
  for (const src of sources) {
    const info = classifyDictFile(baseName(src.path));
    if (!info || isJunkPath(src.path)) continue;
    const dir = dirName(src.path);
    // FAT names are case-insensitive, so are the reader's lookups of .dict next to .idx.
    // Per archive: two archives holding the same path are two dictionaries, and
    // merging them could pair one's index with the other's data.
    const key = (src.origin || '') + '\n' + dir.toLowerCase() + '/' + info.stem.toLowerCase();
    if (!groups.has(key)) groups.set(key, { dir, stem: info.stem, origin: src.origin, files: {}, skipped: [] });
    const g = groups.get(key);
    const entry = { path: src.path, blob: src.blob, unpack: info.unpack };
    const have = g.files[info.kind];
    // A plain file beats its compressed twin: nothing to unpack.
    if (!have || (have.unpack && !entry.unpack)) {
      if (have) g.skipped.push({ path: have.path, kind: info.kind });
      g.files[info.kind] = entry;
    } else {
      g.skipped.push({ path: entry.path, kind: info.kind });
    }
  }
  const dicts = [];
  const stray = [];
  for (const g of groups.values()) {
    if (g.files.idx) {
      dicts.push(g);
    } else {
      for (const kind of Object.keys(g.files)) stray.push(g.files[kind].path);
    }
  }
  return { dicts, stray };
}

// Checks one group and works out what to upload. Returns { errors, warnings,
// ifo, title, words, folder, stem, uploads } where each upload has `unpack`
// true for a file the reader cannot use packed, 'offer' for a dictzip .dict.dz
// (the user decides), and false for a plain file.
async function analyseDictionary(group, fallbackName) {
  const errors = [];
  const warnings = [];
  let ifo = null;
  if (group.files.ifo) {
    ifo = parseIfo(await group.files.ifo.blob.text());
    if (ifo.idxoffsetbits === '64') {
      errors.push('It uses 64-bit index offsets (idxoffsetbits=64), which the reader does not support.');
    }
  } else {
    warnings.push('No .ifo file: the dictionary works, but has no title or word count.');
  }
  if (!group.files.dict) errors.push('No .dict or .dict.dz file next to ' + baseName(group.files.idx.path) + '.');

  const folder =
    sanitizeFolderName(ifo && ifo.bookname) || sanitizeFolderName(fallbackName) || sanitizeFolderName(group.stem) || 'dictionary';
  const stem = isSafeStem(group.stem) ? group.stem : folder.replace(/ /g, '_');

  for (const skipped of group.skipped) {
    warnings.push('Not used: ' + baseName(skipped.path) + ' (using ' + baseName(group.files[skipped.kind].path) + ').');
  }

  const uploads = [];
  for (const kind of UPLOAD_ORDER) {
    const f = group.files[kind];
    if (!f) continue;
    let unpack = f.unpack === 'always';
    if (f.unpack === 'offer') {
      const dz = await dictzipKind(f.blob);
      if (dz === 'ok') {
        unpack = 'offer';
      } else {
        unpack = true;
        warnings.push(baseName(f.path) + (dz === 'plain' ? ' is plain gzip rather than dictzip' :
          ' has a random-access table the reader cannot use (too large, or an unknown version)') + ', so it is unpacked to .dict.');
      }
    }
    uploads.push({
      kind,
      source: f,
      unpack,
      packedSize: f.blob.size,
      unpackedSize: f.unpack ? await gzipOriginalSize(f.blob) : f.blob.size,
      // The name on the card once unpacked (or when plain), and the one a
      // .dict.dz keeps when the user leaves it packed.
      name: stem + '.' + kind,
      packedName: stem + '.dict.dz',
    });
  }
  if (ifo && ifo.idxfilesize && group.files.idx && !group.files.idx.unpack) {
    if (Number(ifo.idxfilesize) !== group.files.idx.blob.size) {
      warnings.push('The .idx is ' + group.files.idx.blob.size + ' bytes but the .ifo says ' + ifo.idxfilesize + ': the files may not belong together.');
    }
  }
  return {
    errors,
    warnings,
    ifo,
    title: (ifo && ifo.bookname) || group.stem,
    words: ifo && ifo.wordcount ? Number(ifo.wordcount) : null,
    folder,
    stem,
    uploads,
  };
}

// --- installed dictionaries ---------------------------------------------------

// The reader's verdict on the folder root/folderName (root is /dictionaries or
// /.dictionaries), from its listing: DictionaryRegistry::findStem, plus the
// setting's and Dictionary::open()'s length limits.
// { level: 'ok'|'warn'|'error', messages, stem, ifoName }.
function evaluateInstalled(root, folderName, entries) {
  const messages = [];
  const files = entries.filter((e) => !e.isDirectory && !e.name.startsWith('._'));
  const has = (name) => files.some((e) => e.name.toLowerCase() === name.toLowerCase());
  if (folderName.startsWith('.')) {
    return { level: 'error', messages: ['The reader skips folders whose name starts with a dot.'], stem: null };
  }
  // Exactly ".idx", as the reader compares it.
  const stems = [...new Set(files.filter((e) => e.name.length > 4 && e.name.endsWith('.idx')).map((e) => e.name.slice(0, -4)))];
  if (stems.length === 0) {
    if (files.some((e) => /\.idx\.gz$/i.test(e.name))) {
      messages.push('The index is compressed (.idx.gz). The reader needs a plain .idx: install the dictionary again from this page, which unpacks it.');
    } else if (files.some((e) => /\.idx$/i.test(e.name))) {
      messages.push('The index file must end in lower-case ".idx".');
    } else {
      messages.push('No .idx index file.');
    }
    return { level: 'error', messages, stem: null };
  }
  if (stems.length > 1) {
    messages.push('Several dictionaries in one folder (' + stems.join(', ') + '). The reader skips the folder: give each its own folder.');
    return { level: 'error', messages, stem: null };
  }
  const stem = stems[0];
  const ifoName = has(stem + '.ifo') ? files.find((e) => e.name.toLowerCase() === (stem + '.ifo').toLowerCase()).name : null;
  if (folderName.length > MAX_FOLDER_NAME) {
    messages.push('The folder name is longer than ' + MAX_FOLDER_NAME + ' characters. The reader keeps only that many when you choose it, and then cannot find the folder: rename it.');
    return { level: 'error', messages, stem, ifoName };
  }
  if ((root + '/' + folderName + '/' + stem).length > MAX_BASE_PATH) {
    messages.push('The folder and file names together are too long for the reader (' + MAX_BASE_PATH + ' characters with the path): shorten them.');
    return { level: 'error', messages, stem, ifoName };
  }
  if (!has(stem + '.dict') && !has(stem + '.dict.dz')) {
    messages.push('No ' + stem + '.dict or ' + stem + '.dict.dz next to the index.');
    return { level: 'error', messages, stem, ifoName };
  }
  let level = 'ok';
  if (!has(stem + '.dict')) {
    level = 'warn';
    messages.push('Uses the compressed .dict.dz. It works, but a plain .dict needs no 32 KB inflate buffer per lookup, which a busy reading heap cannot always spare.');
  }
  if (!has(stem + '.syn') && has(stem + '.syn.gz')) {
    level = 'warn';
    messages.push('The synonyms file is compressed (.syn.gz) and is ignored.');
  }
  return { level, messages, stem, ifoName };
}
