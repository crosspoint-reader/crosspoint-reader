// Tests src/network/html/js/epub-fonts.js: fonts embedded in an EPUB come out byte for byte, whether
// stored plain, obfuscated (IDPF and Adobe algorithms) or as WOFF, with family and style read from
// the font; real encryption is refused.
//
//   node test/web/epub_fonts_test.mjs <dir with node_modules/jsdom and node_modules/jszip>
//
// The fonts are the tracked Noto Sans sources under lib/EpdFont/builtinFonts/source.
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/epub_fonts_test.mjs <dir with node_modules/jsdom and node_modules/jszip>');
  process.exit(2);
}
const require = createRequire(join(deps, 'noop.js'));
const { JSDOM } = require('jsdom');
const JSZip = require('jszip');

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const font = (style) => readFileSync(join(repo, 'lib/EpdFont/builtinFonts/source/NotoSans', `NotoSans-${style}.ttf`));

globalThis.window = globalThis;
globalThis.DOMParser = new JSDOM('').window.DOMParser;
vm.runInThisContext(readFileSync(join(repo, 'src/network/html/js/epub-fonts.js'), 'utf8'));
const { EpubFonts } = globalThis;

let failures = 0;
const check = (ok, what) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
};
const sha = (buf) => createHash('sha1').update(Buffer.from(buf)).digest('hex');

// Every table's tag and bytes: what unpacking a WOFF must preserve. The directory around them is
// rebuilt, so offsets and padding may differ from the original file.
function tables(buf) {
  const b = Buffer.from(buf);
  const out = {};
  for (let i = 0; i < b.readUInt16BE(4); i++) {
    const o = 12 + i * 16;
    out[b.toString('latin1', o, o + 4)] = sha(b.subarray(b.readUInt32BE(o + 8), b.readUInt32BE(o + 8) + b.readUInt32BE(o + 12)));
  }
  return JSON.stringify(Object.entries(out).sort());
}

// What a publisher does to obfuscate: XOR the start of the file with the key.
const xor = (buf, key, n) => {
  const out = Buffer.from(buf);
  for (let i = 0; i < Math.min(n, out.length); i++) out[i] ^= key[i % key.length];
  return out;
};

// A minimal WOFF 1.0 encoder: every table zlib-compressed.
function woff(sfnt) {
  const view = new DataView(sfnt.buffer, sfnt.byteOffset, sfnt.byteLength);
  const count = view.getUint16(4);
  const tables = [];
  for (let i = 0; i < count; i++) {
    const o = 12 + i * 16;
    const offset = view.getUint32(o + 8), length = view.getUint32(o + 12);
    const data = sfnt.subarray(offset, offset + length);
    const comp = deflateSync(data);
    tables.push({ tag: sfnt.subarray(o, o + 4), checksum: view.getUint32(o + 4), orig: data,
      stored: comp.length < data.length ? comp : data });
  }
  let size = 44 + count * 20;
  const offsets = tables.map((t) => { const at = size; size += (t.stored.length + 3) & ~3; return at; });
  const out = Buffer.alloc(size);
  out.write('wOFF', 0, 'latin1');
  out.writeUInt32BE(view.getUint32(0), 4);
  out.writeUInt32BE(size, 8);
  out.writeUInt16BE(count, 12);
  tables.forEach((t, i) => {
    const o = 44 + i * 20;
    t.tag.copy(out, o);
    out.writeUInt32BE(offsets[i], o + 4);
    out.writeUInt32BE(t.stored.length, o + 8);
    out.writeUInt32BE(t.orig.length, o + 12);
    out.writeUInt32BE(t.checksum, o + 16);
    t.stored.copy(out, offsets[i]);
  });
  return out;
}

const UNIQUE_ID = ' urn:isbn:978-0-00-000000-0 ';  // with whitespace, which the key leaves out
const UUID = 'urn:uuid:0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0';
const idpfKey = createHash('sha1').update(UNIQUE_ID.replace(/[ \t\r\n]/g, '')).digest();
const adobeKey = Buffer.from(UUID.slice(9).replace(/-/g, ''), 'hex');

const originals = { Regular: font('Regular'), Bold: font('Bold'), Italic: font('Italic'), BoldItalic: font('BoldItalic') };
const zip = new JSZip();
zip.file('mimetype', 'application/epub+zip');
zip.file('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" ' +
  'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" ' +
  'media-type="application/oebps-package+xml"/></rootfiles></container>');
zip.file('OEBPS/content.opf', `<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0"
  unique-identifier="bookid"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="bookid">${UNIQUE_ID}</dc:identifier><dc:identifier>${UUID}</dc:identifier>
  <dc:title>Fonts</dc:title></metadata><manifest>
  <item id="f1" href="fonts/regular.ttf" media-type="font/ttf"/>
  <item id="f2" href="fonts/bold.otf" media-type="application/vnd.ms-opentype"/>
  <item id="f3" href="fonts/italic.ttf" media-type="font/ttf"/>
  <item id="f4" href="fonts/bolditalic.woff" media-type="font/woff"/>
  <item id="f5" href="fonts/locked.ttf" media-type="font/ttf"/>
  </manifest><spine/></package>`);
zip.file('OEBPS/fonts/regular.ttf', originals.Regular);
zip.file('OEBPS/fonts/bold.otf', xor(originals.Bold, idpfKey, 1040));
zip.file('OEBPS/fonts/italic.ttf', xor(originals.Italic, adobeKey, 1024));
zip.file('OEBPS/fonts/bolditalic.woff', woff(originals.BoldItalic));
zip.file('OEBPS/fonts/locked.ttf', xor(originals.Regular, Buffer.from([7, 7, 7]), 4096));
zip.file('META-INF/encryption.xml', `<?xml version="1.0"?><encryption
  xmlns="urn:oasis:names:tc:opendocument:xmlns:container" xmlns:enc="http://www.w3.org/2001/04/xmlenc#">
  <enc:EncryptedData><enc:EncryptionMethod Algorithm="http://www.idpf.org/2008/embedding"/>
    <enc:CipherData><enc:CipherReference URI="OEBPS/fonts/bold.otf"/></enc:CipherData></enc:EncryptedData>
  <enc:EncryptedData><enc:EncryptionMethod Algorithm="http://ns.adobe.com/pdf/enc#RC"/>
    <enc:CipherData><enc:CipherReference URI="OEBPS/fonts/italic.ttf"/></enc:CipherData></enc:EncryptedData>
  <enc:EncryptedData><enc:EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes128-cbc"/>
    <enc:CipherData><enc:CipherReference URI="OEBPS/fonts/locked.ttf"/></enc:CipherData></enc:EncryptedData>
</encryption>`);

const book = await JSZip.loadAsync(await zip.generateAsync({ type: 'uint8array' }));
const { fonts, skipped } = await EpubFonts.extract(book);
const byPath = Object.fromEntries(fonts.map((f) => [f.path.split('/').pop(), f]));

for (const text of ['', 'abc', 'x'.repeat(1000)]) {
  check(Buffer.from(EpubFonts.sha1(new TextEncoder().encode(text))).toString('hex') ===
    createHash('sha1').update(text).digest('hex'), `SHA-1 of ${text.length} bytes matches node:crypto`);
}
check(fonts.length === 4, `four fonts extracted (${fonts.length})`);
const expect = [['regular.ttf', 'Regular', 'regular'], ['bold.otf', 'Bold', 'bold'],
  ['italic.ttf', 'Italic', 'italic'], ['bolditalic.woff', 'BoldItalic', 'bolditalic']];
for (const [name, original, style] of expect) {
  const f = byPath[name];
  if (name.endsWith('.woff')) {
    check(!!f && tables(f.data) === tables(originals[original]), `${name}: every table equals the source font's`);
  } else {
    check(!!f && sha(f.data) === sha(originals[original]), `${name}: bytes equal the source font`);
  }
  check(!!f && f.family === 'Noto Sans' && f.style === style, `${name}: Noto Sans ${style} (${f && f.family} ${f && f.style})`);
}
check(skipped.length === 1 && /locked\.ttf$/.test(skipped[0].path) && /DRM/.test(skipped[0].reason),
  'the AES-encrypted font is refused as DRM');
const families = EpubFonts.byFamily(fonts);
check(families.length === 1 && Object.keys(families[0].styles).sort().join() === 'bold,bolditalic,italic,regular',
  'byFamily groups the four styles under one family');

console.log(failures ? `${failures} FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
