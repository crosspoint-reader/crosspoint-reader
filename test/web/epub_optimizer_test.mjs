#!/usr/bin/env node
// Tests for the EPUB optimizer in the web File Manager (src/network/html/FilesPage.html).
//
// The optimizer runs in the browser at upload ("Optimize EPUB" in the upload dialog). This
// harness loads the page into jsdom, runs the page's own inline script — no copy of the code —
// and drives convertEpubFile() and its text-side helpers on real and synthetic EPUBs:
//
//   * DRM: an encrypted book is refused, a font-obfuscated one is accepted untouched;
//   * XHTML comments and invisible code points are removed (soft hyphens kept);
//   * empty chapter stubs are collapsed out of the spine and links to them redirected;
//   * long chapters are split under the size limits, the parts listed in order in the OPF
//     spine and manifest, no text lost, and every internal link, anchor and TOC entry (NCX
//     and nav) still resolving to an existing file and id;
//   * optimizing an optimized book again loses nothing.
//
// Images are not checked: processImage needs a canvas, which jsdom has not, so the harness
// swaps it for a pass-through that keeps each image's bytes.
//
// jsdom and jszip are not repository dependencies; install them once into a folder outside the
// repo (CI does the same, at the versions pinned in .github/workflows/ci.yml):
//   npm install --prefix /tmp/web-deps --no-save jsdom@24.1.3 jszip@3.10.1
// Run, from anywhere:
//   node test/web/epub_optimizer_test.mjs /tmp/web-deps [-v] [more.epub ...]
// The first argument is the folder holding node_modules/jsdom and node_modules/jszip. Without
// book arguments the real-book checks run on test/epubs/*.epub and test/fixtures/*.epub; EPUB
// paths given instead get the same checks (any book on hand, it is read and never changed).
// -v prints the page's conversion log for every book. Exit status 0 means every check passed.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const HTML_DIR = path.join(ROOT, 'src', 'network', 'html');

// ---------------------------------------------------------------------------------------------
// Arguments and dependencies
// ---------------------------------------------------------------------------------------------

const verbose = process.argv.includes('-v') || process.argv.includes('--verbose');
const [modulesDir, ...bookArgs] = process.argv.slice(2).filter((a) => a !== '-v' && a !== '--verbose');
if (!modulesDir) {
  console.error('usage: node test/web/epub_optimizer_test.mjs <dir with node_modules/jsdom and node_modules/jszip> [-v] [book.epub ...]');
  process.exit(2);
}
const requireFromModules = createRequire(path.join(path.resolve(modulesDir), 'noop.js'));
const { JSDOM, VirtualConsole } = requireFromModules('jsdom');
const JSZip = requireFromModules('jszip');

// ---------------------------------------------------------------------------------------------
// Tiny test runner
// ---------------------------------------------------------------------------------------------

let failures = 0;
let checks = 0;
const notes = [];

function check(condition, message) {
  checks++;
  if (!condition) {
    failures++;
    console.log(`    FAIL ${message}`);
  }
  return !!condition;
}

function note(message) {
  notes.push(message);
  console.log(`    note ${message}`);
}

async function test(name, fn) {
  console.log(`- ${name}`);
  try {
    await fn();
  } catch (e) {
    failures++;
    console.log(`    FAIL threw: ${e && e.stack ? e.stack.split('\n').slice(0, 4).join('\n      ') : e}`);
  }
}

// ---------------------------------------------------------------------------------------------
// The page under test
// ---------------------------------------------------------------------------------------------

function resolveIncludes(html, baseDir) {
  const includeRe = /<!--\s*#include\s+file="([^"]+)"\s*-->/g;
  for (let depth = 0; depth < 8; depth++) {
    let count = 0;
    html = html.replace(includeRe, (_, rel) => {
      count++;
      return fs.readFileSync(path.join(baseDir, rel), 'utf8');
    });
    if (count === 0) break;
  }
  return html;
}

// Loads FilesPage.html into jsdom and runs its main inline script as a classic script, so the
// page's top-level functions and constants are globals of the window's VM context.
function loadPage() {
  const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'FilesPage.html'), 'utf8'), HTML_DIR);
  const inline = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  const main = inline.reduce((a, b) => (b.length > a.length ? b : a), '');
  const virtualConsole = new VirtualConsole();
  if (verbose) {
    // jsdom renamed sendTo() to forwardTo(); accept either.
    if (typeof virtualConsole.forwardTo === 'function') virtualConsole.forwardTo(console, { jsdomErrors: 'none' });
    else virtualConsole.sendTo(console, { omitJSDOMErrors: true });
  }
  // runScripts 'outside-only': the page's script elements stay inert; the harness runs them.
  const dom = new JSDOM(html, { url: 'http://127.0.0.1:8770/files', runScripts: 'outside-only', virtualConsole });
  const w = dom.window;
  // No device behind the page: every request fails fast, as on a dropped connection. The page's
  // start-up (listing, plugins) then gives up quietly; what it leaves unhandled is ignored.
  w.fetch = () => Promise.reject(new w.TypeError('Failed to fetch (no device in the test harness)'));
  w.alert = () => {};
  process.on('unhandledRejection', (reason) => {
    if (verbose) console.log(`    (page start-up: ${reason && reason.message ? reason.message : reason})`);
  });
  // JSZip schedules its work with setImmediate, polyfilled over window.postMessage when absent;
  // jsdom's message events never satisfy that polyfill, so hand it Node's.
  w.setImmediate = setImmediate;
  w.clearImmediate = clearImmediate;
  const ctx = dom.getInternalVMContext();
  const run = (code, filename = 'harness.js') => new vm.Script(code, { filename }).runInContext(ctx);
  // Older jsdom (CI pins 24.x) has no TextEncoder/TextDecoder, which every browser has. Node's
  // do the work; the wrappers hand back the window's own Uint8Array, as JSZip type-checks it.
  w.__hostTextEncoder = new TextEncoder();
  w.__HostTextDecoder = TextDecoder;
  run(`if (typeof TextEncoder === 'undefined') {
    window.TextEncoder = class TextEncoder {
      get encoding() { return 'utf-8'; }
      encode(text = '') { const bytes = __hostTextEncoder.encode(String(text)); const out = new Uint8Array(bytes.length); out.set(bytes); return out; }
    };
  }
  if (typeof TextDecoder === 'undefined') {
    window.TextDecoder = class TextDecoder {
      constructor(label = 'utf-8', options = {}) { this.decoder = new __HostTextDecoder(label, options); }
      get encoding() { return this.decoder.encoding; }
      decode(input, options) { return this.decoder.decode(input, options); }
    };
  }`, 'polyfills.js');
  run(fs.readFileSync(path.join(HTML_DIR, 'js', 'jszip.min.js'), 'utf8'), 'jszip.min.js');
  run(main, 'FilesPage.inline.js');
  // jsdom has no canvas: images keep their bytes under the name the optimizer gives them. The
  // calls are recorded so a test can see what the optimizer asked for.
  run(`var processImageCalls = [];
  processImage = async function (data, imageState = 0, imagePath = '', preserveColor = false) {
    processImageCalls.push({ imagePath, imageState, preserveColor });
    return {
      parts: [{ data, suffix: '', width: 0, height: 0, size: data.byteLength }],
      meta: { origW: 0, origH: 0, origSize: data.byteLength, wasSplit: false, rotated: false,
              finalW: 0, finalH: 0, finalSize: data.byteLength, imageState, preserveColor },
    };
  };`);
  return { dom, w, run };
}

const page = loadPage();
const { w, run } = page;

function pageLogText() {
  return [...w.document.querySelectorAll('#log-container .log-entry')].map((e) => e.textContent).join('\n');
}

function setToggle(id, value) {
  const el = w.document.getElementById(id);
  if (el) el.checked = value;
  return !!el;
}

// A Blob's bytes; older jsdom has no Blob.arrayBuffer(), so FileReader then.
function blobBytes(blob) {
  if (typeof blob.arrayBuffer === 'function') return blob.arrayBuffer();
  return new Promise((resolve, reject) => {
    const reader = new w.FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

// Runs the page's optimizer on EPUB bytes; returns { bytes, log } or throws what it threw.
async function optimize(bytes, name, { split = true, colorCover = false } = {}) {
  setToggle('splitLongSectionsToggle', split);
  setToggle('preserveCoverColorToggle', colorCover);
  run('processImageCalls.length = 0');
  const file = new w.File([new w.Uint8Array(bytes)], name, { type: 'application/epub+zip' });
  const blob = await w.convertEpubFile(file, null);
  const out = new Uint8Array(await blobBytes(blob));
  const log = pageLogText();
  if (verbose) console.log(log.split('\n').map((l) => `      | ${l}`).join('\n'));
  return { bytes: out, log };
}

// ---------------------------------------------------------------------------------------------
// EPUB reading
// ---------------------------------------------------------------------------------------------

const XHTML_RE = /\.(xhtml|html|htm)$/i;
const SPLIT_SUFFIX_RE = /__ci_section_\d{3}(?:_\d+)?(?=\.[^.]+$)/i;

function parseXml(text, type = 'application/xml') {
  const doc = new w.DOMParser().parseFromString(text, type);
  return doc.querySelector('parsererror') ? null : doc;
}

function decodeHref(href) {
  try { return decodeURIComponent(href); } catch (e) { return href; }
}

function resolveZipPath(fromFile, href) {
  if (href.startsWith('/')) return normalizeZipPath(href.substring(1));
  const dir = fromFile.includes('/') ? fromFile.substring(0, fromFile.lastIndexOf('/') + 1) : '';
  return normalizeZipPath(dir + href);
}

function normalizeZipPath(p) {
  const out = [];
  for (const part of p.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') out.pop();
    else out.push(part);
  }
  return out.join('/');
}

function basenameOf(p) {
  return p.substring(p.lastIndexOf('/') + 1);
}

function stripBom(text) {
  return text.charCodeAt(0) === 0xfeff ? text.substring(1) : text;
}

async function readBook(bytes) {
  const zip = await JSZip.loadAsync(bytes);
  const texts = new Map();
  const text = async (p) => {
    if (!texts.has(p)) texts.set(p, zip.files[p] ? stripBom(await zip.files[p].async('string')) : null);
    return texts.get(p);
  };
  const paths = Object.keys(zip.files).filter((p) => !zip.files[p].dir);
  const container = await text('META-INF/container.xml');
  const rootMatch = container && container.match(/full-path=["']([^"']+)["']/);
  const opfPath = rootMatch ? rootMatch[1] : paths.find((p) => p.endsWith('.opf'));
  const opfText = await text(opfPath);
  const opfDoc = parseXml(opfText);
  const manifest = new Map();
  const spine = [];
  let ncxPath = null;
  let navPath = null;
  if (opfDoc) {
    for (const item of opfDoc.getElementsByTagNameNS('*', 'item')) {
      const href = item.getAttribute('href') || '';
      const entry = {
        id: item.getAttribute('id'),
        href,
        path: resolveZipPath(opfPath, decodeHref(href.split('#')[0])),
        mediaType: item.getAttribute('media-type') || '',
        properties: item.getAttribute('properties') || '',
      };
      manifest.set(entry.id, entry);
      if (entry.mediaType === 'application/x-dtbncx+xml') ncxPath = entry.path;
      if (entry.properties.split(/\s+/).includes('nav')) navPath = entry.path;
    }
    for (const ref of opfDoc.getElementsByTagNameNS('*', 'itemref')) {
      const item = manifest.get(ref.getAttribute('idref'));
      spine.push(item ? item.path : `<missing idref ${ref.getAttribute('idref')}>`);
    }
  }
  return { zip, paths, text, opfPath, opfText, opfDoc, manifest, spine, ncxPath, navPath };
}

async function xhtmlDoc(book, p) {
  const t = await book.text(p);
  if (t === null) return null;
  return parseXml(t, 'application/xhtml+xml') || new w.DOMParser().parseFromString(t, 'text/html');
}

// Visible text of one document, whitespace-collapsed.
async function bodyText(book, p) {
  const doc = await xhtmlDoc(book, p);
  if (!doc) return '';
  const root = doc.body || doc.documentElement;
  const clone = root.cloneNode(true);
  for (const el of [...clone.getElementsByTagName('*')]) {
    if (['script', 'style'].includes(el.localName)) el.remove();
  }
  return (clone.textContent || '').replace(/\s+/g, ' ').trim();
}

async function anchorsOf(book, p) {
  const doc = await xhtmlDoc(book, p);
  const ids = new Set();
  if (!doc) return ids;
  for (const el of doc.getElementsByTagName('*')) {
    const id = el.getAttribute('id');
    if (id) ids.add(id);
    if (el.localName === 'a' && el.getAttribute('name')) ids.add(el.getAttribute('name'));
  }
  return ids;
}

// Every internal reference of the book: XHTML href/src/xlink:href, NCX content@src, OPF guide.
// Returns [{ source, attr, value, target, fragment, problem }], problem null when it resolves.
async function collectLinks(book) {
  const links = [];
  const anchorCache = new Map();
  const anchors = async (p) => {
    if (!anchorCache.has(p)) anchorCache.set(p, await anchorsOf(book, p));
    return anchorCache.get(p);
  };
  const add = async (source, attr, value) => {
    if (!value || /^[a-z][a-z0-9+.-]*:/i.test(value)) return;
    const hashAt = value.indexOf('#');
    const filePart = hashAt < 0 ? value : value.substring(0, hashAt);
    const fragment = hashAt < 0 ? '' : decodeHref(value.substring(hashAt + 1));
    const target = filePart ? resolveZipPath(source, decodeHref(filePart)) : source;
    let problem = null;
    if (!book.zip.files[target]) problem = 'missing file';
    else if (fragment && XHTML_RE.test(target) && !(await anchors(target)).has(fragment)) problem = 'missing anchor';
    links.push({ source, attr, value, target, fragment, problem });
  };
  for (const p of book.paths.filter((x) => XHTML_RE.test(x))) {
    const doc = await xhtmlDoc(book, p);
    if (!doc) continue;
    for (const el of doc.getElementsByTagName('*')) {
      for (const attr of ['href', 'src', 'xlink:href']) {
        if (el.localName === 'link' && attr === 'href') continue; // stylesheets: checked by the reader, not links
        await add(p, attr, el.getAttribute(attr));
      }
    }
  }
  if (book.ncxPath && book.zip.files[book.ncxPath]) {
    const ncx = parseXml(await book.text(book.ncxPath));
    if (ncx) for (const c of ncx.getElementsByTagNameNS('*', 'content')) await add(book.ncxPath, 'src', c.getAttribute('src'));
  }
  if (book.opfDoc) {
    for (const r of book.opfDoc.getElementsByTagNameNS('*', 'reference')) await add(book.opfPath, 'href', r.getAttribute('href'));
  }
  return links;
}

// A link's identity across optimization: split parts fold back onto their original file and
// converted images onto their extension-less name, so a link broken after optimizing can be
// matched against the same link before it.
function linkKey(link) {
  const fold = (p) => p.replace(SPLIT_SUFFIX_RE, '').replace(/\.(png|gif|webp|bmp|jpe?g)$/i, '.img');
  return `${fold(link.source)} -> ${fold(link.target)}#${link.fragment}`;
}

// ---------------------------------------------------------------------------------------------
// Building synthetic EPUBs
// ---------------------------------------------------------------------------------------------

async function buildEpub(files) {
  const zip = new JSZip();
  zip.file('mimetype', 'application/epub+zip', { compression: 'STORE' });
  zip.file('META-INF/container.xml',
    '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">' +
    '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>');
  for (const [p, content] of Object.entries(files)) zip.file(p, content);
  return zip.generateAsync({ type: 'uint8array' });
}

function xhtml(title, body, { bodyAttrs = '', head = '' } = {}) {
  return '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n' +
    '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">\n' +
    `<head><title>${title}</title>${head}</head>\n<body${bodyAttrs}>\n${body}\n</body>\n</html>\n`;
}

function opf(items, spineIds, { guide = '' } = {}) {
  const manifest = items.map(([id, href, type, props]) =>
    `<item id="${id}" href="${href}" media-type="${type}"${props ? ` properties="${props}"` : ''}/>`).join('\n    ');
  const spine = spineIds.map((id) => `<itemref idref="${id}"/>`).join('\n    ');
  return '<?xml version="1.0" encoding="utf-8"?>\n' +
    '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">\n' +
    '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="uid">urn:uuid:12345678-1234-1234-1234-123456789abc</dc:identifier>' +
    '<dc:title>Test</dc:title><dc:language>en</dc:language></metadata>\n' +
    `  <manifest>\n    ${manifest}\n  </manifest>\n  <spine toc="ncx">\n    ${spine}\n  </spine>\n${guide}</package>\n`;
}

function ncx(points) {
  const navPoints = points.map(([label, src], i) =>
    `<navPoint id="np${i + 1}" playOrder="${i + 1}"><navLabel><text>${label}</text></navLabel><content src="${src}"/></navPoint>`).join('\n    ');
  return '<?xml version="1.0" encoding="utf-8"?>\n<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">\n' +
    '  <head><meta name="dtb:uid" content="urn:uuid:12345678-1234-1234-1234-123456789abc"/></head>\n' +
    `  <docTitle><text>Test</text></docTitle>\n  <navMap>\n    ${navPoints}\n  </navMap>\n</ncx>\n`;
}

function nav(points) {
  const lis = points.map(([label, href]) => `<li><a href="${href}">${label}</a></li>`).join('');
  return xhtml('Contents', `<nav epub:type="toc" id="toc"><ol>${lis}</ol></nav>`);
}

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

const NOT_DRM_FONT = new Uint8Array(4096).map((_, i) => (i * 37 + 11) & 0xff);

function encryptionXml(entries) {
  return '<?xml version="1.0" encoding="UTF-8"?>\n<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container" ' +
    'xmlns:enc="http://www.w3.org/2001/04/xmlenc#">\n' +
    entries.map(([algorithm, uri]) =>
      `  <enc:EncryptedData><enc:EncryptionMethod Algorithm="${algorithm}"/>` +
      `<enc:CipherData><enc:CipherReference URI="${uri}"/></enc:CipherData></enc:EncryptedData>`).join('\n') +
    '\n</encryption>\n';
}

async function smallBook(extraFiles = {}, extraItems = []) {
  return buildEpub({
    'OEBPS/content.opf': opf([['ch1', 'ch1.xhtml', 'application/xhtml+xml'], ['ncx', 'toc.ncx', 'application/x-dtbncx+xml'],
      ...extraItems], ['ch1']),
    'OEBPS/toc.ncx': ncx([['One', 'ch1.xhtml']]),
    'OEBPS/ch1.xhtml': xhtml('One', '<p>Hello.</p>'),
    ...extraFiles,
  });
}

await test('DRM: content encryption is refused', async () => {
  const bytes = await smallBook({
    'META-INF/encryption.xml': encryptionXml([['http://www.w3.org/2001/04/xmlenc#aes128-cbc', 'OEBPS/ch1.xhtml']]),
  });
  let error = null;
  try { await optimize(bytes, 'drm.epub'); } catch (e) { error = e; }
  check(error && /DRM-protected/.test(error.message), `refused with the DRM message (got ${error ? error.message : 'no error'})`);
});

await test('DRM: unreadable encryption metadata is refused', async () => {
  const bytes = await smallBook({ 'META-INF/encryption.xml': '<encryption><EncryptedData></encryption>' });
  let error = null;
  try { await optimize(bytes, 'bad-enc.epub'); } catch (e) { error = e; }
  check(error && /invalid encryption metadata/.test(error.message), `refused (got ${error ? error.message : 'no error'})`);
});

await test('DRM: font obfuscation is accepted and the font passes through', async () => {
  const enc = encryptionXml([
    ['http://www.idpf.org/2008/embedding', 'OEBPS/fonts/serif.otf'],
    ['http://ns.adobe.com/pdf/enc#RC', 'OEBPS/fonts/sans.dat'],
  ]);
  const bytes = await smallBook(
    { 'META-INF/encryption.xml': enc, 'OEBPS/fonts/serif.otf': NOT_DRM_FONT, 'OEBPS/fonts/sans.dat': NOT_DRM_FONT },
    [['f1', 'fonts/serif.otf', 'font/otf'], ['f2', 'fonts/sans.dat', 'application/vnd.ms-opentype']]);
  const { bytes: out } = await optimize(bytes, 'fonts.epub');
  const book = await readBook(out);
  for (const p of ['OEBPS/fonts/serif.otf', 'OEBPS/fonts/sans.dat']) {
    const data = book.zip.files[p] ? await book.zip.files[p].async('uint8array') : null;
    check(data && Buffer.compare(Buffer.from(data), Buffer.from(NOT_DRM_FONT)) === 0, `${p} unchanged`);
  }
  check((await book.text('META-INF/encryption.xml')) === enc, 'encryption.xml unchanged');
});

await test('comments and invisible code points are removed, soft hyphens kept', async () => {
  const body = '<!-- a comment -->\n<p>zero​width‌ non‍joiner﻿ &#x200B;&#8203;ref ' +
    'soft­hyphen hy&#173;phen &#xAD;x</p>\n<!-- second\n multi-line comment -->\n<p>end</p>';
  const head = '<style type="text/css"><![CDATA[ p { margin: 0 } /* <!-- kept: inside CDATA --> */ ]]></style>';
  const bytes = await smallBook({ 'OEBPS/ch1.xhtml': xhtml('One​', body, { head }) });
  const { bytes: out, log } = await optimize(bytes, 'scrub.epub');
  const book = await readBook(out);
  const t = await book.text('OEBPS/ch1.xhtml');
  check(!/<!-- a comment|multi-line comment/.test(t), 'comments outside CDATA removed');
  check(t.includes('<!-- kept: inside CDATA -->'), 'CDATA section kept verbatim');
  check(!/[​‌‍﻿]/.test(t) && !/&#x200B;|&#8203;/i.test(t), 'zero-width characters and their references removed');
  check(t.includes('soft­hyphen') && t.includes('hy&#173;phen') && t.includes('&#xAD;x'), 'soft hyphens kept, literal and as references');
  check(/<title>One<\/title>/.test(t), 'title scrubbed too');
  check(/Comments \(2, /.test(log), 'log reports the comments');
  check(/Blank codepoints: ch1\.xhtml \(\d+ removed\)/.test(log), 'log reports the code points');
  // The helpers on their own.
  const r = run(`stripComments('<?xml version="1.0"?><!DOCTYPE x [<!ENTITY e "<!-- not -->">]><x><!--c--><?pi <!-- pi --> ?></x>')`);
  check(r.count === 1 && r.text.includes('<!-- not -->') && r.text.includes('<!-- pi -->') && !r.text.includes('<!--c-->'),
    'stripComments keeps DOCTYPE subset and processing instructions');
  check(run('isScrubbedBlankCodepoint(0xAD)') === false && run('isScrubbedBlankCodepoint(0x200B)') === true &&
    run('isScrubbedBlankCodepoint(0x09)') === false && run('isScrubbedBlankCodepoint(0x0A)') === false,
    'isScrubbedBlankCodepoint: soft hyphen, tab and newline are not scrubbed');
});

await test('empty chapter stubs are collapsed and links follow them', async () => {
  const items = [
    ['intro', 'Text/intro.xhtml', 'application/xhtml+xml'],
    ['stub', 'Text/stub.xhtml', 'application/xhtml+xml'],
    ['c1', 'Text/c1.xhtml', 'application/xhtml+xml'],
    ['stub2', 'Text/stub2.xhtml', 'application/xhtml+xml'],
    ['c2', 'Text/c2.xhtml', 'application/xhtml+xml'],
    ['pic', 'Text/pic.xhtml', 'application/xhtml+xml'],
    ['c3', 'Text/c3.xhtml', 'application/xhtml+xml'],
    ['nav', 'nav.xhtml', 'application/xhtml+xml', 'nav'],
    ['ncx', 'toc.ncx', 'application/x-dtbncx+xml'],
  ];
  const bytes = await buildEpub({
    'OEBPS/content.opf': opf(items, ['intro', 'stub', 'c1', 'stub2', 'c2', 'pic', 'c3']),
    'OEBPS/toc.ncx': ncx([['Intro', 'Text/intro.xhtml'], ['One', 'Text/stub.xhtml'], ['Two', 'Text/stub2.xhtml'], ['Three', 'Text/pic.xhtml']]),
    'OEBPS/nav.xhtml': nav([['One', 'Text/stub.xhtml#start'], ['Two', 'Text/stub2.xhtml']]),
    'OEBPS/Text/intro.xhtml': xhtml('Intro', '<p>See <a href="stub.xhtml">one</a> and <a href="stub.xhtml#start">its start</a>.</p>'),
    // Same body id and title as the next file, nothing to show: collapsed.
    'OEBPS/Text/stub.xhtml': xhtml('Chapter 1', '<div><a id="start"></a><style>p{}</style></div>', { bodyAttrs: ' id="c1"' }),
    'OEBPS/Text/c1.xhtml': xhtml('Chapter 1', '<p id="start">One begins.</p>', { bodyAttrs: ' id="c1"' }),
    // Empty, but titled differently from the next file: kept.
    'OEBPS/Text/stub2.xhtml': xhtml('Chapter 2 cover', '<div></div>', { bodyAttrs: ' id="c2"' }),
    'OEBPS/Text/c2.xhtml': xhtml('Chapter 2', '<p>Two.</p>', { bodyAttrs: ' id="c2"' }),
    // Only an image, which is content: kept.
    'OEBPS/Text/pic.xhtml': xhtml('Chapter 3', '<div><img src="../a.png" alt=""/></div>', { bodyAttrs: ' id="c3"' }),
    'OEBPS/Text/c3.xhtml': xhtml('Chapter 3', '<p>Three.</p>', { bodyAttrs: ' id="c3"' }),
    'OEBPS/a.png': new Uint8Array([0x89, 0x50, 0x4e, 0x47]),
  });
  const { bytes: out, log } = await optimize(bytes, 'stubs.epub');
  const book = await readBook(out);
  check(JSON.stringify(book.spine.map((p) => basenameOf(p))) ===
    JSON.stringify(['intro.xhtml', 'c1.xhtml', 'stub2.xhtml', 'c2.xhtml', 'pic.xhtml', 'c3.xhtml']),
    `spine drops only the stub (got ${book.spine.map((p) => basenameOf(p)).join(', ')})`);
  check([...book.manifest.values()].some((i) => i.path === 'OEBPS/Text/stub.xhtml'), 'the stub stays in the manifest');
  const introText = await book.text('OEBPS/Text/intro.xhtml');
  check(introText.includes('href="c1.xhtml"') && introText.includes('href="c1.xhtml#start"'), 'chapter links follow the stub, fragment kept');
  check((await book.text('OEBPS/toc.ncx')).includes('src="Text/c1.xhtml"'), 'NCX entry follows the stub');
  check((await book.text('OEBPS/toc.ncx')).includes('src="Text/stub2.xhtml"'), 'NCX entry to a kept file untouched');
  check((await book.text('OEBPS/nav.xhtml')).includes('href="Text/c1.xhtml#start"'), 'nav entry follows the stub');
  check(/Collapsed 1 empty chapter stubs/.test(log), 'log reports the collapse');
});

await test('the cover can keep its color; other images cannot', async () => {
  const items = [
    ['cover-img', 'Images/cover.jpg', 'image/jpeg'],
    ['pic', 'Images/pic.png', 'image/png'],
    ['coverpage', 'Text/titlepage.xhtml', 'application/xhtml+xml'],
    ['c1', 'Text/c1.xhtml', 'application/xhtml+xml'],
    ['ncx', 'toc.ncx', 'application/x-dtbncx+xml'],
  ];
  const bytes = await buildEpub({
    'OEBPS/content.opf': opf(items, ['coverpage', 'c1']).replace('<dc:language>', '<meta name="cover" content="cover-img"/><dc:language>'),
    'OEBPS/toc.ncx': ncx([['One', 'Text/c1.xhtml']]),
    'OEBPS/Text/titlepage.xhtml': xhtml('Cover', '<div><img src="../Images/cover.jpg" alt="cover"/></div>'),
    'OEBPS/Text/c1.xhtml': xhtml('One', '<p><img src="../Images/pic.png" alt=""/></p>'),
    'OEBPS/Images/cover.jpg': new Uint8Array([0xff, 0xd8, 0xff, 0xd9]),
    'OEBPS/Images/pic.png': new Uint8Array([0x89, 0x50, 0x4e, 0x47]),
  });
  const zip = await w.JSZip.loadAsync(new w.Uint8Array(bytes));
  const found = [...await w.findEpubCoverImagePaths(zip)];
  check(JSON.stringify(found) === JSON.stringify(['OEBPS/Images/cover.jpg']), `the cover image is found (${found.join(', ')})`);

  await optimize(bytes, 'cover.epub', { colorCover: false });
  check(run('processImageCalls').every((c) => !c.preserveColor), 'option off: every image goes to grayscale');
  const { log } = await optimize(bytes, 'cover.epub', { colorCover: true });
  const calls = run('processImageCalls');
  check(calls.find((c) => c.imagePath === 'OEBPS/Images/cover.jpg')?.preserveColor === true, 'option on: the cover keeps its color');
  check(calls.find((c) => c.imagePath === 'OEBPS/Images/pic.png')?.preserveColor === false, 'option on: other images still go to grayscale');
  check(/cover\.jpg kept in color \(cover\)/.test(log), 'log says the cover kept its color');
});

// --- Long chapters ---------------------------------------------------------------------------

const LOREM = ('Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut ' +
  'labore et dolore magna aliqua Ut enim ad minim veniam quis nostrud exercitation ullamco laboris nisi ut ' +
  'aliquip ex ea commodo consequat Duis aute irure dolor in reprehenderit in voluptate velit esse cillum').split(' ');

function words(n, seed) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(LOREM[(seed * 7 + i * 13) % LOREM.length]);
  return out.join(' ');
}

// One long chapter in a wrapper div: 30 sections of 20 paragraphs, footnote references through
// the text with the notes (and their back-links) at the end, a table and an <hr> on the way.
function longChapter() {
  const parts = ['<div class="chapter" id="chap">', '<h1 id="top">The Long Chapter</h1>'];
  let note = 0;
  for (let s = 1; s <= 30; s++) {
    parts.push(`<h2 id="sec${s}">Section ${s}</h2>`);
    for (let p = 1; p <= 20; p++) {
      const id = `p${s}-${p}`;
      let ref = '';
      if (p % 7 === 0) {
        note++;
        ref = ` <a epub:type="noteref" href="#fn${note}" id="ref${note}">${note}</a>`;
      }
      parts.push(`<p id="${id}">${words(55, s * 100 + p)}${ref}.</p>`);
    }
    parts.push(`<p class="back"><a href="#sec${s}">Back to section ${s}</a></p>`);
    if (s === 12) parts.push('<table id="t1"><tr><td>' + words(40, 1) + '</td><td>' + words(40, 2) + '</td></tr></table>');
    if (s === 20) parts.push('<hr/>');
  }
  parts.push('<div class="notes" epub:type="footnotes">');
  for (let n = 1; n <= note; n++) parts.push(`<aside epub:type="footnote" id="fn${n}"><p><a href="#ref${n}">${n}</a> ${words(12, n)}.</p></aside>`);
  parts.push('</div>', '</div>');
  return { body: parts.join('\n'), notes: note };
}

// A long appendix of plain paragraphs, marked linear="no".
function longAppendix() {
  const ps = [];
  for (let i = 1; i <= 160; i++) ps.push(`<p id="a${i}">${words(50, i)}.</p>`);
  return ps.join('\n');
}

async function longBook() {
  const { body } = longChapter();
  const items = [
    ['cover', 'Text/cover.xhtml', 'application/xhtml+xml'],
    ['toc', 'Text/toc.xhtml', 'application/xhtml+xml', 'nav'],
    ['long', 'Text/long.xhtml', 'application/xhtml+xml'],
    ['other', 'Text/other.xhtml', 'application/xhtml+xml'],
    ['appx', 'Text/appendix.xhtml', 'application/xhtml+xml'],
    ['ncx', 'toc.ncx', 'application/x-dtbncx+xml'],
    ['css', 'Styles/style.css', 'text/css'],
  ];
  // A navigation document long enough to be split if it were treated like a chapter.
  const navEntries = [['Start', 'long.xhtml']];
  for (let s = 1; s <= 30; s++) for (let p = 1; p <= 20; p++) navEntries.push([`Section ${s}, paragraph ${p}`, `long.xhtml#p${s}-${p}`]);
  const guide = '  <guide><reference type="text" title="Start" href="Text/long.xhtml#sec5"/></guide>\n';
  const opfText = opf(items, ['cover', 'toc', 'long', 'other', 'appx'], { guide })
    .replace('<itemref idref="appx"/>', '<itemref idref="appx" linear="no"/>');
  return buildEpub({
    'OEBPS/content.opf': opfText,
    'OEBPS/toc.ncx': ncx([['Start', 'Text/long.xhtml'], ['Section 10', 'Text/long.xhtml#sec10'],
      ['Section 29', 'Text/long.xhtml#sec29'], ['Note 40', 'Text/long.xhtml#fn40'], ['Other', 'Text/other.xhtml']]),
    'OEBPS/Text/toc.xhtml': nav(navEntries),
    'OEBPS/Styles/style.css': 'p { text-indent: 1em }',
    'OEBPS/Text/cover.xhtml': xhtml('Cover', '<p>Cover</p>'),
    'OEBPS/Text/long.xhtml': xhtml('The Long Chapter', body, { head: '<link rel="stylesheet" type="text/css" href="../Styles/style.css"/>' }),
    'OEBPS/Text/other.xhtml': xhtml('Other', '<p>See <a href="long.xhtml#sec25">section 25</a>, <a href="long.xhtml#fn3">note 3</a>, ' +
      '<a href="long.xhtml">the start</a> and <a href="long.xhtml#top">the top</a>.</p>'),
    'OEBPS/Text/appendix.xhtml': xhtml('Appendix', longAppendix()),
  });
}

function spineIndexOfParts(spine, chapterPath) {
  const base = chapterPath.replace(/\.[^.]+$/, '');
  return spine.map((p, i) => (p === chapterPath || p.startsWith(`${base}__ci_section_`) ? i : -1)).filter((i) => i >= 0);
}

const SECTION_LIMITS = () => ({
  hard: run('SECTION_SPLIT_HARD_BYTE_LIMIT'),
  bytes: run('SECTION_SPLIT_BYTE_THRESHOLD'),
  words: run('SECTION_SPLIT_WORD_THRESHOLD'),
  lookahead: run('SECTION_SPLIT_NATURAL_BOUNDARY_LOOKAHEAD_BYTES'),
});

async function spineText(book) {
  const texts = [];
  for (const p of book.spine) texts.push(await bodyText(book, p));
  return texts.join(' ');
}

const squash = (t) => t.replace(/\s+/g, '');

await test('long chapters are split under the limits, in order, with every link intact', async () => {
  const limits = SECTION_LIMITS();
  check(limits.hard === 49152 && limits.bytes === 32768 && limits.words === 8000, 'limits are 8,000 words / 32 KB preferred, 48 KB hard');
  const bytes = await longBook();
  const unsplit = await readBook((await optimize(bytes, 'long.epub', { split: false })).bytes);
  const { bytes: outBytes, log } = await optimize(bytes, 'long.epub', { split: true });
  const book = await readBook(outBytes);

  // Spine and manifest: the chapter, then its parts, in order, each listed once.
  const longParts = spineIndexOfParts(book.spine, 'OEBPS/Text/long.xhtml');
  check(longParts.length >= 6, `the long chapter became several parts (${longParts.length})`);
  check(longParts.every((idx, k) => idx === longParts[0] + k), 'its parts follow each other in the spine');
  const partPaths = longParts.map((i) => book.spine[i]);
  check(partPaths[0] === 'OEBPS/Text/long.xhtml', 'the first part keeps the chapter\'s path');
  check(partPaths.slice(1).every((p, k) => p === `OEBPS/Text/long__ci_section_${String(k + 2).padStart(3, '0')}.xhtml`),
    `parts are numbered in reading order (${partPaths.map(basenameOf).join(', ')})`);
  const manifestPaths = [...book.manifest.values()].map((i) => i.path);
  check(partPaths.every((p) => manifestPaths.filter((m) => m === p).length === 1), 'every part has exactly one manifest item');
  check(partPaths.every((p) => [...book.manifest.values()].find((i) => i.path === p).mediaType === 'application/xhtml+xml'),
    'parts are application/xhtml+xml');
  check(JSON.stringify(book.spine.filter((p) => !partPaths.includes(p) && !p.includes('appendix'))) ===
    JSON.stringify(unsplit.spine.filter((p) => !p.includes('long.xhtml') && !p.includes('appendix'))),
    'the rest of the spine is unchanged');

  // Sizes.
  let largest = 0;
  for (const p of partPaths) {
    const t = await book.text(p);
    const size = Buffer.byteLength(t, 'utf8');
    largest = Math.max(largest, size);
    check(size <= limits.hard, `${basenameOf(p)} is ${size} bytes, within the hard limit`);
    const w = run(`countSectionWords(extractSectionText(${JSON.stringify(t)}))`);
    check(w <= limits.words, `${basenameOf(p)} has ${w} words, within the word limit`);
    check(parseXml(t, 'application/xhtml+xml') !== null, `${basenameOf(p)} is well-formed XHTML`);
    check(/<link rel="stylesheet"[^>]*style\.css/.test(t) && /<title>The Long Chapter<\/title>/.test(t), `${basenameOf(p)} keeps the head`);
  }
  check(largest <= limits.bytes + limits.lookahead, `parts stay within preferred size + lookahead (largest ${largest})`);

  // Natural breaks: each later part starts at a section heading (or at the notes block, which
  // follows the last section's paragraphs without one).
  for (const p of partPaths.slice(1)) {
    const doc = await xhtmlDoc(book, p);
    const first = doc.getElementById('chap')?.firstElementChild;
    const natural = first && (/^h[1-6]$/.test(first.localName) || first.getAttribute('class') === 'notes');
    check(natural, `${basenameOf(p)} starts at a heading (${first ? first.localName + '#' + first.id : 'none'})`);
  }
  // Tables are never cut.
  const tableIn = [];
  for (const p of partPaths) if ((await xhtmlDoc(book, p)).getElementById('t1')) tableIn.push(p);
  check(tableIn.length === 1, 'the table sits whole in one part');

  // Nothing lost.
  const before = squash(await bodyText(unsplit, 'OEBPS/Text/long.xhtml'));
  const after = squash((await Promise.all(partPaths.map((p) => bodyText(book, p)))).join(''));
  check(before === after, `the parts hold exactly the chapter's text (${before.length} vs ${after.length} chars)`);
  const idsBefore = await anchorsOf(unsplit, 'OEBPS/Text/long.xhtml');
  const idsAfter = new Set();
  for (const p of partPaths) for (const id of await anchorsOf(book, p)) idsAfter.add(id);
  check([...idsBefore].every((id) => idsAfter.has(id)), 'every id of the chapter is in some part');

  // Links: all of them resolve, and the ones into the chapter reach the part holding the anchor.
  const links = await collectLinks(book);
  const broken = links.filter((l) => l.problem);
  check(broken.length === 0, `no broken links (${broken.slice(0, 5).map((l) => `${basenameOf(l.source)}: ${l.value} (${l.problem})`).join('; ')})`);
  const intoParts = links.filter((l) => l.fragment && partPaths.slice(1).includes(l.target));
  check(intoParts.some((l) => l.source === book.ncxPath), 'NCX entries were pointed into later parts');
  check(intoParts.some((l) => l.source.endsWith('toc.xhtml')), 'nav entries were pointed into later parts');
  check(intoParts.some((l) => l.source === book.opfPath), 'the OPF guide was pointed into a later part');
  check(intoParts.some((l) => l.source.endsWith('other.xhtml')), 'links from other chapters were pointed into later parts');
  const noteRefs = links.filter((l) => /^fn\d+$/.test(l.fragment) && partPaths.includes(l.source));
  check(noteRefs.length > 10 && noteRefs.every((l) => !l.problem), `note references from every part reach their notes (${noteRefs.length})`);
  check(noteRefs.some((l) => l.source !== partPaths[0] && l.target !== l.source), 'including references from later parts to the notes part');
  const backLinks = links.filter((l) => partPaths.includes(l.source) && /^sec\d+$/.test(l.fragment));
  check(backLinks.length === 30 && backLinks.every((l) => l.value.startsWith('#') === (l.target === l.source)),
    `links within one part stay bare "#id" links, those across parts name the part (${backLinks.filter((l) => !l.value.startsWith('#')).length} of ${backLinks.length} crossed)`);
  const otherText = await book.text('OEBPS/Text/other.xhtml');
  check(otherText.includes('href="long.xhtml"') && otherText.includes('href="long.xhtml#top"'),
    'links to the chapter start and to anchors in its first part are left as written');

  // The navigation document is never split, even in the spine and long.
  check(!book.paths.some((p) => p.includes('toc__ci_section')), 'the nav document was not split');
  check(Buffer.byteLength(await book.text('OEBPS/Text/toc.xhtml'), 'utf8') > limits.bytes, '(it was long enough to be)');

  // linear="no" carries over to the appendix's parts.
  const appxParts = spineIndexOfParts(book.spine, 'OEBPS/Text/appendix.xhtml');
  const refs = [...book.opfDoc.getElementsByTagNameNS('*', 'itemref')];
  check(appxParts.length >= 2 && appxParts.every((i) => refs[i].getAttribute('linear') === 'no'),
    `the non-linear appendix's ${appxParts.length} parts are all linear="no"`);

  check(/EPUB sections: \d+ parts from 2 long sections/.test(log), 'log reports the split');

  // Splitting switched off leaves the chapter alone.
  check(unsplit.spine.filter((p) => p.includes('__ci_section_')).length === 0, 'with the option off, nothing is split');

  // Optimizing the optimized book again: nothing overwritten, nothing lost, links intact.
  const again = await readBook((await optimize(outBytes, 'long.epub', { split: true })).bytes);
  check(book.paths.every((p) => again.zip.files[p]), 'a second pass keeps every file of the first');
  check(squash(await spineText(again)) === squash(await spineText(book)), 'a second pass keeps the text and its order');
  const againBroken = (await collectLinks(again)).filter((l) => l.problem);
  check(againBroken.length === 0, `a second pass breaks no link (${againBroken.slice(0, 3).map((l) => l.value).join('; ')})`);
});

await test('a re-split part never overwrites an existing part', async () => {
  // A chapter that a previous pass left at 40 KB (allowed by the lookahead) is split again; its
  // second part must not take the name of the part the previous pass already wrote.
  const big = [];
  for (let i = 1; i <= 120; i++) big.push(`<p id="q${i}">${words(50, i)}.</p>`);
  const items = [['c', 'c.xhtml', 'application/xhtml+xml'], ['c2', 'c__ci_section_002.xhtml', 'application/xhtml+xml'],
    ['ncx', 'toc.ncx', 'application/x-dtbncx+xml']];
  const bytes = await buildEpub({
    'OEBPS/content.opf': opf(items, ['c', 'c2']),
    'OEBPS/toc.ncx': ncx([['C', 'c.xhtml'], ['C2', 'c__ci_section_002.xhtml#z1']]),
    'OEBPS/c.xhtml': xhtml('C', big.join('\n')),
    'OEBPS/c__ci_section_002.xhtml': xhtml('C', '<p id="z1">The second part from before.</p>'),
  });
  const book = await readBook((await optimize(bytes, 'resplit.epub')).bytes);
  check((await book.text('OEBPS/c__ci_section_002.xhtml')).includes('The second part from before.'), 'the existing part is intact');
  check(book.spine[book.spine.length - 1] === 'OEBPS/c__ci_section_002.xhtml', 'and still last in the spine');
  check(book.spine.length >= 3 && book.spine.slice(1, -1).every((p) => /c__ci_section_002_\d+\.xhtml$/.test(p)),
    `the new parts got free names (${book.spine.map(basenameOf).join(', ')})`);
});

await test('a chapter held in one big wrapper next to its heading is cut inside the wrapper', async () => {
  // <h2/> + <div class="text">everything</div>: the shape Calibre and many publishers produce.
  const ps = [];
  for (let i = 1; i <= 260; i++) ps.push(`<p id="w${i}">${words(50, i)}.</p>`);
  const letter = [];
  for (let i = 1; i <= 120; i++) letter.push(`<p id="l${i}">${words(50, 1000 + i)}.</p>`);
  const body = '<h2 id="head">The Chapter</h2>\n<div class="text" id="txt">\n' + ps.slice(0, 130).join('\n') +
    '\n<blockquote class="letter" id="letter">\n' + letter.join('\n') + '\n</blockquote>\n' + ps.slice(130).join('\n') + '\n</div>';
  const list = [];
  for (let i = 1; i <= 300; i++) list.push(`<li>${words(30, i)}</li>`);
  const items = [['w', 'w.xhtml', 'application/xhtml+xml'], ['l', 'list.xhtml', 'application/xhtml+xml'],
    ['o', 'o.xhtml', 'application/xhtml+xml'], ['ncx', 'toc.ncx', 'application/x-dtbncx+xml']];
  const bytes = await buildEpub({
    'OEBPS/content.opf': opf(items, ['w', 'l', 'o']),
    'OEBPS/toc.ncx': ncx([['W', 'w.xhtml#head'], ['Letter', 'w.xhtml#letter'], ['End', 'w.xhtml#w260'], ['List', 'list.xhtml']]),
    'OEBPS/w.xhtml': xhtml('W', body),
    'OEBPS/list.xhtml': xhtml('List', `<h2>A list</h2>\n<ol id="ol">\n${list.join('\n')}\n</ol>`),
    'OEBPS/o.xhtml': xhtml('O', '<p><a href="w.xhtml#w200">200</a> <a href="w.xhtml#l60">letter 60</a> <a href="w.xhtml#txt">text</a></p>'),
  });
  const unsplit = await readBook((await optimize(bytes, 'wrapper.epub', { split: false })).bytes);
  const book = await readBook((await optimize(bytes, 'wrapper.epub')).bytes);
  const limits = SECTION_LIMITS();
  const partPaths = spineIndexOfParts(book.spine, 'OEBPS/w.xhtml').map((i) => book.spine[i]);
  check(partPaths.length >= 4, `the chapter was cut (${partPaths.length} parts)`);
  for (const p of partPaths) {
    const size = Buffer.byteLength(await book.text(p), 'utf8');
    check(size <= limits.hard, `${basenameOf(p)} is ${size} bytes, within the hard limit`);
  }
  for (const p of partPaths.slice(1)) {
    const doc = await xhtmlDoc(book, p);
    const first = [...doc.body.children][0];
    check(first && first.localName === 'div' && first.getAttribute('class') === 'text',
      `${basenameOf(p)} wraps its share in a copy of the wrapper`);
  }
  const letterParts = [];
  for (const p of partPaths) if ((await xhtmlDoc(book, p)).querySelector('blockquote.letter p')) letterParts.push(p);
  check(letterParts.length >= 2, `the long letter was cut too, each piece still in its blockquote (${letterParts.length} parts)`);
  check(squash(await bodyText(unsplit, 'OEBPS/w.xhtml')) ===
    squash((await Promise.all(partPaths.map((p) => bodyText(book, p)))).join('')), 'no text lost or reordered');
  const broken = (await collectLinks(book)).filter((l) => l.problem);
  check(broken.length === 0, `no broken links (${broken.slice(0, 3).map((l) => l.value).join('; ')})`);
  const o = await book.text('OEBPS/o.xhtml');
  check(/href="w__ci_section_\d{3}\.xhtml#w200"/.test(o) && o.includes('href="w.xhtml#txt"'),
    'links follow their anchors; the wrapper\'s own id stays in the first part');
  check(spineIndexOfParts(book.spine, 'OEBPS/list.xhtml').length === 1, 'a long list is not opened (its numbering would restart)');
});

// --- Upload options --------------------------------------------------------------------------

await test('upload options are remembered only when asked, and survive storage failures', async () => {
  const doc = w.document;
  const $ = (id) => doc.getElementById(id);
  const key = run('UPLOAD_SETTINGS_STORAGE_KEY');
  w.localStorage.clear();

  w.openUploadModal();
  check(!$('convertBeforeUpload').checked && $('splitLongSectionsToggle').checked && !$('preserveCoverColorToggle').checked &&
    !$('rememberUploadSettings').checked && run('JPEG_QUALITY') === 85, 'a first visit opens with the defaults');

  $('convertBeforeUpload').checked = true;
  w.toggleConvertOptions();
  w.setQualityPreset(60);
  w.setOverlap(10);
  w.setHandedness('left');
  w.setDeviceTarget('X3');
  $('preserveCoverColorToggle').checked = true;
  w.updateUploadSettingsPersistence();
  $('splitLongSectionsToggle').checked = false;
  w.updateUploadSettingsPersistence();
  check(w.localStorage.getItem(key) === null, 'nothing is stored while Remember is off');

  $('rememberUploadSettings').checked = true;
  w.updateUploadSettingsPersistence();
  const stored = JSON.parse(w.localStorage.getItem(key) || 'null');
  check(stored && stored.rememberSettings && stored.convertBeforeUpload && stored.quality === 60 && stored.overlap === 10 &&
    stored.handedness === 'left' && stored.deviceTarget === 'X3' && stored.preserveCoverColor && !stored.splitLongSections,
    `switching Remember on stores every option (${JSON.stringify(stored)})`);

  w.closeUploadModal();
  check(!$('convertBeforeUpload').checked && run('JPEG_QUALITY') === 85 && run('DEVICE_TARGET') === 'auto',
    'closing the dialog puts the defaults back on the page');
  check(w.localStorage.getItem(key) !== null, '... and keeps what is stored');
  doc.dispatchEvent(new w.Event('DOMContentLoaded'));
  check(w.localStorage.getItem(key) !== null, 'page start-up does not wipe what is stored');

  w.openUploadModal();
  check($('convertBeforeUpload').checked && $('uploadBtn').textContent === 'Optimize & Upload', 'the next visit opens with Optimize EPUB ticked');
  check(run('JPEG_QUALITY') === 60 && run('OVERLAP_PERCENT') === 10 && run('HANDEDNESS') === 'left' && run('DEVICE_TARGET') === 'X3',
    'quality, overlap, rotation and device come back');
  check($('preserveCoverColorToggle').checked && !$('splitLongSectionsToggle').checked && $('rememberUploadSettings').checked,
    'the switches come back');

  $('rememberUploadSettings').checked = false;
  w.updateUploadSettingsPersistence();
  check(w.localStorage.getItem(key) === null, 'switching Remember off forgets them');

  // Storage that throws (private window, blocked site data): the defaults, and no exception.
  w.localStorage.setItem(key, '{not json');
  w.closeUploadModal();
  let threw = null;
  try { w.openUploadModal(); } catch (e) { threw = e; }
  check(!threw && !$('convertBeforeUpload').checked, 'unreadable stored options fall back to the defaults');
  w.localStorage.clear();
  const descriptor = Object.getOwnPropertyDescriptor(w, 'localStorage');
  Object.defineProperty(w, 'localStorage', { configurable: true, get() { throw new w.DOMException('blocked', 'SecurityError'); } });
  try {
    w.closeUploadModal();
    w.openUploadModal();
    $('rememberUploadSettings').checked = true;
    w.updateUploadSettingsPersistence();
    w.setQualityPreset(45);
    threw = null;
  } catch (e) {
    threw = e;
  } finally {
    if (descriptor) Object.defineProperty(w, 'localStorage', descriptor);
    else delete w.localStorage;
  }
  check(!threw, `blocked storage throws nothing (${threw ? threw.message : 'ok'})`);
  w.closeUploadModal();
});

// --- Real books --------------------------------------------------------------------------------

function brokenKeys(links) {
  return new Set(links.filter((l) => l.problem).map(linkKey));
}

// The same checks for any book: optimize with splitting off and on, then compare. What the
// original book already had broken may stay broken; nothing new may break.
async function realBookChecks(file) {
  const name = path.basename(file);
  const bytes = fs.readFileSync(file);
  const input = await readBook(bytes);
  let off;
  try {
    off = await readBook((await optimize(bytes, name, { split: false })).bytes);
  } catch (e) {
    note(`${name}: not optimized: ${e.message}`);
    return;
  }
  const { bytes: onBytes, log } = await optimize(bytes, name, { split: true });
  const on = await readBook(onBytes);
  const limits = SECTION_LIMITS();

  check(on.opfDoc !== null, `${name}: the OPF parses`);
  const inputPaths = new Set(input.paths.map((p) => p.replace(/\.(png|gif|webp|bmp|jpe?g)$/i, '.img')));
  const missing = [...on.manifest.values()].filter((i) => !on.zip.files[i.path] &&
    inputPaths.has(i.path.replace(/\.(png|gif|webp|bmp|jpe?g)$/i, '.img')));
  check(missing.length === 0, `${name}: every manifest item is in the zip (${missing.slice(0, 3).map((i) => i.href).join(', ')})`);
  check(on.spine.every((p) => on.zip.files[p] || !input.zip.files[p]), `${name}: every spine item is in the zip`);
  check(new Set(on.spine).size === on.spine.length || new Set(input.spine).size !== input.spine.length, `${name}: no spine item listed twice`);

  // Splitting moves text between files and nothing else.
  check(squash(await spineText(on)) === squash(await spineText(off)), `${name}: splitting keeps the text and its order`);

  // Links, NCX and nav entries included.
  const brokenIn = brokenKeys(await collectLinks(input));
  const offLinks = await collectLinks(off);
  const onLinks = await collectLinks(on);
  const newOff = offLinks.filter((l) => l.problem && !brokenIn.has(linkKey(l)));
  check(newOff.length === 0, `${name}: optimizing breaks no link (${newOff.slice(0, 3).map((l) => `${basenameOf(l.source)}: ${l.value} ${l.problem}`).join('; ')})`);
  const brokenOff = brokenKeys(offLinks);
  const newOn = onLinks.filter((l) => l.problem && !brokenOff.has(linkKey(l)) && !brokenIn.has(linkKey(l)));
  check(newOn.length === 0, `${name}: splitting breaks no link (${newOn.slice(0, 3).map((l) => `${basenameOf(l.source)}: ${l.value} ${l.problem}`).join('; ')})`);
  const tocOn = onLinks.filter((l) => l.source === on.ncxPath || l.source === on.navPath);
  check(tocOn.filter((l) => l.problem && !brokenOff.has(linkKey(l))).length === 0, `${name}: the TOC (${tocOn.length} entries) resolves`);

  // Sizes: what was split must be within the hard limit; what is still over it is reported.
  const parts = on.spine.filter((p) => SPLIT_SUFFIX_RE.test(p));
  const chapters = new Set(parts.map((p) => p.replace(SPLIT_SUFFIX_RE, '')));
  let largestBefore = 0;
  let largestAfter = 0;
  for (const p of off.spine) largestBefore = Math.max(largestBefore, Buffer.byteLength((await off.text(p)) || '', 'utf8'));
  const overLimit = [];
  for (const p of on.spine) {
    const size = Buffer.byteLength((await on.text(p)) || '', 'utf8');
    largestAfter = Math.max(largestAfter, size);
    if (size > limits.hard) overLimit.push(`${basenameOf(p)} ${Math.round(size / 1024)} KB`);
  }
  for (const p of [...chapters, ...parts]) {
    const size = Buffer.byteLength((await on.text(p)) || '', 'utf8');
    check(size <= limits.hard, `${name}: split file ${basenameOf(p)} is ${size} bytes, within the hard limit`);
  }
  const summary = `${name}: ${chapters.size ? `${chapters.size} chapters split into ${chapters.size + parts.length} files` : 'nothing split'}; largest spine file ` +
    `${Math.round(largestBefore / 1024)} KB -> ${Math.round(largestAfter / 1024)} KB` +
    (overLimit.length ? `; still over 48 KB (could not be cut): ${overLimit.join(', ')}` : '');
  console.log(`    ${summary}`);
  if (overLimit.length) note(summary);
  if (parts.length) check(/EPUB sections: \d+ parts from \d+ long sections/.test(log), `${name}: log reports the split`);

  // A second pass over the optimized book changes nothing that matters.
  const again = await readBook((await optimize(onBytes, name, { split: true })).bytes);
  check(on.paths.every((p) => again.zip.files[p]), `${name}: a second pass keeps every file`);
  check(squash(await spineText(again)) === squash(await spineText(on)), `${name}: a second pass keeps the text`);
  const brokenOn = brokenKeys(onLinks);
  const newAgain = (await collectLinks(again)).filter((l) => l.problem && !brokenOn.has(linkKey(l)));
  check(newAgain.length === 0, `${name}: a second pass breaks no link (${newAgain.slice(0, 3).map((l) => l.value).join('; ')})`);
}

await test('Auto sizes pictures for the connected reader, whatever its model', async () => {
  const answer = (status) => async () => ({ ok: true, status: 200, json: async () => status });
  w.fetch = answer({ version: 't', deviceType: 'X4 Pro', displayWidth: 480, displayHeight: 800 });
  await w.fetchVersion();
  run("setDeviceTarget('auto')");
  check(run('MAX_WIDTH') === 480 && run('MAX_HEIGHT') === 800, `X4 Pro: 480x800 (got ${run('MAX_WIDTH')}x${run('MAX_HEIGHT')})`);
  w.fetch = answer({ version: 't', deviceType: 'T5S3', displayWidth: 960, displayHeight: 540 });
  await w.fetchVersion();
  check(run('MAX_WIDTH') === 540 && run('MAX_HEIGHT') === 960, `a landscape-reported panel is sized upright (got ${run('MAX_WIDTH')}x${run('MAX_HEIGHT')})`);
  w.fetch = answer({ version: 't', deviceType: 'X3', displayWidth: 528, displayHeight: 792 });
  await w.fetchVersion();
  check(run('MAX_WIDTH') === 528 && run('MAX_HEIGHT') === 792, 'X3 keeps its profile');
  w.fetch = () => Promise.reject(new w.TypeError('Failed to fetch (no device in the test harness)'));
});

// ---------------------------------------------------------------------------------------------
// In place: the File Manager's Optimize action on books already on the card
// ---------------------------------------------------------------------------------------------

// Runs optimizeSelected() over the given books as if they were selected in the listing, with the
// device answered here: /download hands out each book, /api/book/replace records what it is sent.
async function optimizeInPlace(books) {
  const sent = [];
  w.fetch = async (url, opts = {}) => {
    const u = new URL(url, 'http://127.0.0.1:8770');
    if (u.pathname === '/download') {
      const book = books.find((b) => b.path === u.searchParams.get('path'));
      if (!book) return { ok: false, status: 404 };
      return { ok: true, status: 200, blob: async () => new w.Blob([new w.Uint8Array(book.bytes)]) };
    }
    if (u.pathname === '/api/book/replace') {
      sent.push({ path: u.searchParams.get('path'), keep: u.searchParams.get('keep'), bytes: new Uint8Array(await blobBytes(opts.body)) });
      return { ok: true, status: 200, json: async () => ({ ok: true }) };
    }
    throw new w.TypeError('unexpected request ' + url);
  };
  w.document.getElementById('file-table').innerHTML = books.map((b) =>
    `<input type="checkbox" class="select-item" checked data-path="${encodeURIComponent(b.path)}" data-name="${b.path.split('/').pop()}" data-type="file">`).join('');
  setToggle('splitLongSectionsToggle', true);
  run('openOptimizeSelectedModal()');
  await w.optimizeSelected();
  const states = books.map((_, i) => w.document.getElementById('optimizeState' + i).textContent);
  w.fetch = () => Promise.reject(new w.TypeError('Failed to fetch (no device in the test harness)'));
  return { sent, states };
}

await test('in place: a book whose chapters stay as they are keeps its reading state', async () => {
  const { sent, states } = await optimizeInPlace([{ path: '/Books/small.epub', bytes: await smallBook() }]);
  check(sent.length === 1 && sent[0].path === '/Books/small.epub', 'the optimized copy goes back to the same path');
  check(sent[0] && sent[0].keep === '1', `keep=1 when nothing was split or collapsed (got ${sent[0] && sent[0].keep})`);
  const book = sent[0] && await readBook(sent[0].bytes);
  check(!!book && book.spine.length === 1, 'what is sent is a readable EPUB');
  check(/→/.test(states[0]) && !/reset/.test(states[0]), `the row shows the sizes (${states[0]})`);
});

await test('in place: once the run is over, Done closes the dialog', async () => {
  await optimizeInPlace([{ path: '/Books/small.epub', bytes: await smallBook() }]);
  const done = w.document.getElementById('optimizeStartBtn');
  const close = w.document.getElementById('optimizeCancelBtn');
  const modal = w.document.getElementById('optimizeModal');
  check(done.textContent === 'Done' && !done.disabled, `the start button turns into an enabled Done (${done.textContent}, disabled=${done.disabled})`);
  check(close.hidden, 'and stands alone: the Cancel button is hidden');
  await w.optimizeSelected();  // its onclick
  check(!modal.classList.contains('open'), 'Done closes the dialog');
  run('openOptimizeSelectedModal()');
  check(done.textContent === 'Optimize' && !done.disabled && !close.hidden && close.textContent === 'Cancel',
    'opening it again starts afresh');
  run('closeOptimizeModal()');
});

await test('in place: a split book starts afresh', async () => {
  const { sent, states } = await optimizeInPlace([{ path: '/Books/long.epub', bytes: await longBook() }]);
  check(sent.length === 1 && sent[0].keep === '0', `keep=0 when long chapters were split (got ${sent[0] && sent[0].keep})`);
  check(/reading position reset/.test(states[0]), `the row says the position resets (${states[0]})`);
});

await test('in place: a DRM book fails on its own and the batch goes on', async () => {
  const drm = await smallBook({
    'META-INF/encryption.xml': encryptionXml([['http://www.w3.org/2001/04/xmlenc#aes128-cbc', 'OEBPS/ch1.xhtml']]),
  });
  const { sent, states } = await optimizeInPlace([
    { path: '/Books/drm.epub', bytes: drm }, { path: '/Books/small.epub', bytes: await smallBook() }]);
  check(/^Failed: .*DRM-protected/.test(states[0]), `the DRM book fails with the reason (${states[0]})`);
  check(sent.length === 1 && sent[0].path === '/Books/small.epub', 'nothing is sent for it, and the next book is done');
});

const books = bookArgs.length ? bookArgs : [
  ...fs.readdirSync(path.join(ROOT, 'test', 'epubs')).filter((f) => f.endsWith('.epub')).map((f) => path.join(ROOT, 'test', 'epubs', f)),
  ...fs.readdirSync(path.join(ROOT, 'test', 'fixtures')).filter((f) => f.endsWith('.epub')).map((f) => path.join(ROOT, 'test', 'fixtures', f)),
];
for (const file of books.sort()) {
  await test(`real book: ${path.basename(file)}`, () => realBookChecks(file));
}

// ---------------------------------------------------------------------------------------------

console.log(`\n${checks} checks, ${failures} failures${notes.length ? `, ${notes.length} notes` : ''}`);
process.exit(failures ? 1 : 0);
