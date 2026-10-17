#!/usr/bin/env node
// Tests that the web pages send the card file and folder names in NFC (shared/FileNames.js.inc).
// A Mac hands the browser NFD names ("e" + combining accent); sent as they are, they land on the FAT
// card as names Finder cannot see, which match no name typed elsewhere.
//
// Each page is loaded into jsdom and runs its own inline script (includes resolved, no copy of the
// code); fetch, WebSocket and XMLHttpRequest are fakes that record what the page sends:
//
//   * File Manager: a picked file over WebSocket (START line) and over HTTP (multipart filename);
//     a dropped folder's mkdir and destination; New folder; Rename (whose path, the card's own
//     name, must stay as it is); the clash check against an NFD name already on the card;
//   * Font Manager: a .cpfont upload's filename and the family taken from it;
//   * Plugins: the names of the files of a plugin installed from a zip.
//
//   node test/web/upload_names_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/upload_names_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

const HTML_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'src', 'network', 'html');

const NFD = 'Café';  // what a Mac sends: "e" and a combining acute accent
const NFC = 'Café';   // what the card should get: one "é"

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}
const show = (s) => JSON.stringify(s).replace(/[\u0080-￿]/g, (c) => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
function same(actual, expected, what) {
  check(actual === expected, what + (actual === expected ? '' : ` (got ${show(actual)}, want ${show(expected)})`));
}

// Unhandled rejections from a page's start-up (requests the fakes refuse) are not under test.
process.on('unhandledRejection', () => {});

function resolveIncludes(html) {
  for (let depth = 0; depth < 8; depth++) {
    let count = 0;
    html = html.replace(/<!--\s*#include\s+file="([^"]+)"\s*-->/g, (_, rel) => {
      count++;
      return fs.readFileSync(path.join(HTML_DIR, rel), 'utf8');
    });
    if (!count) break;
  }
  return html;
}

function response(status, body) {
  return {
    ok: status >= 200 && status < 300,
    status,
    statusText: String(status),
    json: async () => (typeof body === 'string' ? JSON.parse(body) : body),
    text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
  };
}

// Loads a page with fakes in place of the network, installed before its scripts run, which then run
// as in a browser (external scripts are not fetched). `card` maps a folder to its /api/files listing;
// `answer(url, init)` may answer any other request (undefined: 404). `ws` is 'ok' or 'refused'.
function loadPage(file, { url = 'http://reader.test/', card = {}, answer = () => undefined, ws = 'ok' } = {}) {
  const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, file), 'utf8'));
  const sent = { requests: [], sockets: [], xhrs: [] };
  const beforeParse = (w) => {
    w.fetch = async (u, init = {}) => {
      sent.requests.push({ url: String(u), init });
      const listing = String(u).match(/^\/api\/files\?path=([^&]*)/);
      if (listing) return response(200, card[decodeURIComponent(listing[1])] || []);
      return answer(String(u), init) || response(404, '');
    };
    w.WebSocket = class {
      static OPEN = 1;
      constructor() {
        this.readyState = 1;
        this.bufferedAmount = 0;
        sent.sockets.push(this);
        w.setTimeout(() => (ws === 'refused' ? this.onerror({}) : this.onopen()), 0);
      }
      send(message) {
        if (typeof message === 'string') {
          this.start = message;
          this.size = Number(message.split(':')[2]);
          this.received = 0;
          w.setTimeout(() => this.onmessage({ data: 'READY' }), 0);
        } else {
          this.received += message.byteLength;
          if (this.received >= this.size) w.setTimeout(() => this.onmessage({ data: 'DONE' }), 0);
        }
      }
      close() { this.readyState = 3; }
    };
    w.XMLHttpRequest = class {
      constructor() { this.upload = {}; }
      open(method, u) { this.method = method; this.url = u; }
      send(body) {
        this.body = body;
        this.status = 200;
        this.responseText = 'ok';
        sent.xhrs.push(this);
        w.setTimeout(() => this.onload && this.onload(), 0);
      }
      abort() {}
    };
    w.alert = () => {};
    w.confirm = () => true;
    // Every browser has these; the jsdom CI pins (24.x) has not. Node's text codecs serve here, as
    // nothing under test hands their output to code that checks which window made it.
    if (!w.TextEncoder) w.TextEncoder = TextEncoder;
    if (!w.TextDecoder) w.TextDecoder = TextDecoder;
    if (!w.Blob.prototype.arrayBuffer) {
      w.Blob.prototype.arrayBuffer = function () {
        return new Promise((resolve, reject) => {
          const reader = new w.FileReader();
          reader.onload = () => resolve(reader.result);
          reader.onerror = () => reject(reader.error);
          reader.readAsArrayBuffer(this);
        });
      };
    }
  };
  const dom = new JSDOM(html, { url, runScripts: 'dangerously', virtualConsole: new VirtualConsole(), beforeParse });
  const run = (code) => new vm.Script(code).runInContext(dom.getInternalVMContext());
  return { dom, w: dom.window, run, sent };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 5));
async function until(condition, ms = 3000) {
  for (const end = Date.now() + ms; Date.now() < end; await tick()) if (condition()) return true;
  return condition();
}

function pick(w, id, files) {
  Object.defineProperty(w.document.getElementById(id), 'files', { value: files, configurable: true });
}

// The File Manager's upload of one picked file; resolves once the page reports the upload done.
async function uploadThroughFilesPage(name, options = {}) {
  const page = loadPage('FilesPage.html', { url: 'http://reader.test/files', ...options });
  const { w } = page;
  await tick();
  w.document.getElementById('convertBeforeUpload').checked = false;
  pick(w, 'fileInput', [new w.File(['book bytes'], name, { type: 'application/epub+zip' })]);
  w.uploadFile();
  if (options.clash) {
    await until(() => w.document.getElementById('clashModal').classList.contains('open'));
    w.answerUploadClash(options.clash);
  }
  page.done = await until(() => /All uploads complete|Failed|Skipped/.test(w.document.getElementById('progress-text').textContent));
  return page;
}

const decodedParam = (u, key) => new URL(u, 'http://reader.test').searchParams.get(key);

// ---------------------------------------------------------------------------------------------

console.log('- the helper');
{
  const { w, run, dom } = loadPage('FilesPage.html', { url: 'http://reader.test/files' });
  if (check(typeof w.nfcName === 'function', 'the page has nfcName')) {
    same(w.nfcName(NFD + '.epub'), NFC + '.epub', 'nfcName composes an NFD name');
    same(w.nfcName(NFC + '.epub'), NFC + '.epub', 'and leaves an NFC one as it is');
    same(w.nfcName(undefined), undefined, 'and hands back what is not a string');
    same(run(`(() => {
      const normalize = String.prototype.normalize;
      delete String.prototype.normalize;
      try { return nfcName('${NFD}'); } finally { String.prototype.normalize = normalize; }
    })()`), NFD, 'without String.prototype.normalize the name goes as it is');
  }
  dom.window.close();
}

console.log('- File Manager uploads');
{
  const page = await uploadThroughFilesPage(NFD + '.epub');
  check(page.done && page.sent.sockets.length === 1, 'the upload went over a WebSocket');
  same(page.sent.sockets[0] && page.sent.sockets[0].start, `START:${NFC}.epub:10:/`, 'the START line names the file in NFC');
  page.dom.window.close();
}
{
  const page = await uploadThroughFilesPage(NFD + '.epub', { ws: 'refused' });
  const xhr = page.sent.xhrs.find((x) => x.url.startsWith('/upload'));
  check(page.done && !!xhr, 'with the WebSocket refused, the upload went over HTTP');
  same(xhr && xhr.body.get('file').name, NFC + '.epub', 'the multipart filename is NFC');
  page.dom.window.close();
}
{
  // Uploaded before this fix, the card already holds the NFD name: it looks the same, so it is a clash.
  const page = await uploadThroughFilesPage(NFC + '.epub', {
    card: { '/': [{ name: NFD + '.epub', isDirectory: false }] },
    clash: 'keep',
  });
  same(page.sent.sockets[0] && page.sent.sockets[0].start, `START:${NFC} (2).epub:10:/`,
    'an NFD name on the card counts as a clash; Keep both picks the next free NFC name');
  page.dom.window.close();
}

{
  // Replace: the NFD file is renamed to the upload's NFC name first, so the upload replaces it rather
  // than landing beside it under a name that looks the same.
  const page = await uploadThroughFilesPage(NFC + '.epub', {
    url: 'http://reader.test/files?path=%2Fbooks',
    card: { '/books': [{ name: NFD + '.epub', isDirectory: false }] },
    answer: (u) => (u === '/rename' ? response(200, 'ok') : undefined),
    clash: 'replace',
  });
  const rename = page.sent.requests.find((r) => r.url === '/rename');
  same(rename && rename.init.body.get('path'), '/books/' + NFD + '.epub', 'Replace renames the NFD file on the card');
  same(rename && rename.init.body.get('name'), NFC + '.epub', 'to the upload\'s NFC name');
  same(page.sent.sockets[0] && page.sent.sockets[0].start, `START:${NFC}.epub:10:/books`, 'and then uploads under that name');
  page.dom.window.close();
}
{
  const page = await uploadThroughFilesPage(NFC + '.epub', {
    card: { '/': [{ name: NFC.toUpperCase() + '.EPUB', isDirectory: false }] },
    clash: 'replace',
  });
  check(!page.sent.requests.some((r) => r.url === '/rename'), 'Replace renames nothing when the card has the name in NFC');
  page.dom.window.close();
}

console.log('- File Manager folders and names');
{
  const { w, sent, dom } = loadPage('FilesPage.html', {
    url: 'http://reader.test/files?path=%2Fbooks',
    answer: (u) => (u === '/mkdir' ? response(200, 'ok') : undefined),
  });
  await tick();
  // A dropped folder "Résumé" (NFD) holding one book.
  const book = { isFile: true, isDirectory: false, name: 'x.epub', file: (resolve) => resolve(new w.File(['x'], 'x.epub')) };
  let listed = false;
  const folder = {
    isFile: false, isDirectory: true, name: 'Résumé',
    createReader: () => ({ readEntries: (resolve) => { resolve(listed ? [] : [book]); listed = true; } }),
  };
  const collected = [];
  await w.walkDropEntry(folder, '', collected);
  same(collected[0] && collected[0].relDir, 'Résumé', 'a dropped folder is staged under its NFC name');
  await w.createDroppedFolders(collected);
  const mkdir = sent.requests.find((r) => r.url === '/mkdir');
  same(mkdir && mkdir.init.body.get('name'), 'Résumé', 'and created on the card in NFC');
  same(mkdir && mkdir.init.body.get('path'), '/books', 'inside the folder being browsed');

  w.document.getElementById('folderName').value = NFD;
  w.createFolder();
  const created = sent.xhrs.find((x) => x.url === '/mkdir');
  same(created && created.body.get('name'), NFC, 'New folder sends a typed name in NFC');

  w.document.getElementById('renameItemPath').value = '/books/' + NFD + '.epub';
  w.document.getElementById('renameNewName').value = NFD + ' 2.epub';
  w.confirmRename();
  const renamed = sent.xhrs.find((x) => x.url === '/rename');
  same(renamed && renamed.body.get('name'), NFC + ' 2.epub', 'Rename sends the new name in NFC');
  same(renamed && renamed.body.get('path'), '/books/' + NFD + '.epub', 'and the card\'s own path as it listed it');
  dom.window.close();
}

console.log('- Font Manager');
{
  const { w, sent, dom } = loadPage('FontsPage.html', {
    url: 'http://reader.test/fonts',
    answer: (u) => (u === '/api/fonts/upload' ? response(200, { ok: true }) : u === '/api/fonts' ? response(200, { families: [] }) : undefined),
  });
  await tick();
  pick(w, 'fontFiles', [new w.File(['cpfont'], NFD + '-12.cpfont')]);
  w.document.getElementById('uploadForm').dispatchEvent(new w.Event('submit', { cancelable: true }));
  await until(() => sent.requests.some((r) => r.url === '/api/fonts/upload'));
  const upload = sent.requests.find((r) => r.url === '/api/fonts/upload');
  same(upload && upload.init.body.get('file').name, NFC + '-12.cpfont', 'a .cpfont goes up under its NFC name');
  same(upload && upload.init.body.get('family'), 'Caf_', 'and its family is taken from that name');
  dom.window.close();
}

console.log('- Plugins');
{
  const { w, sent, dom } = loadPage('PluginsPage.html', {
    url: 'http://reader.test/plugins',
    answer: (u) => (u.startsWith('/api/plugins/') ? response(200, {}) : undefined),
  });
  await tick();
  await w.installFiles('demo', 'Demo', [{ name: 'données.json', data: new ArrayBuffer(2) }]);
  const put = sent.requests.find((r) => r.url.startsWith('/api/plugins/file'));
  same(put && decodedParam(put.url, 'file'), 'données.json', 'a plugin file goes onto the card under its NFC name');
  dom.window.close();
}

console.log(failures ? `${failures} check(s) FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
