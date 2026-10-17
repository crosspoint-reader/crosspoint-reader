#!/usr/bin/env node
// Tests that the File Manager's dialogs (src/network/html/FilesPage.html) and its Details dialog
// (js/metadata-editor.js) close on a click beside them only when the click also began there
// (shared/Dialogs.js.inc). Selecting the text of an input and letting go of the button over the
// backdrop reports a click on the backdrop, which used to close the dialog and lose the input.
//
// The page is loaded into jsdom and runs its own scripts (includes resolved, no copy of the code);
// the network is a fake that answers every request with an empty listing or a 404. Each dialog is
// opened and then pressed and released in the four combinations of inside and backdrop.
//
//   node test/web/dialog_backdrop_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/dialog_backdrop_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

const HTML_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'src', 'network', 'html');

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}

// Unhandled rejections from the page's start-up (requests the fake refuses) are not under test.
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

const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'FilesPage.html'), 'utf8'));
const dom = new JSDOM(html, {
  url: 'http://reader.test/files',
  runScripts: 'dangerously',
  virtualConsole: new VirtualConsole(),
  beforeParse(w) {
    w.fetch = async (u) => {
      const ok = String(u).startsWith('/api/files');
      return { ok, status: ok ? 200 : 404, json: async () => [], text: async () => '' };
    };
    w.alert = () => {};
    // Every browser has these; the jsdom CI pins (24.x) has not.
    if (!w.TextEncoder) w.TextEncoder = TextEncoder;
    if (!w.TextDecoder) w.TextDecoder = TextDecoder;
  },
});
const w = dom.window;
const doc = w.document;
const run = (code) => new vm.Script(code).runInContext(dom.getInternalVMContext());

const tick = () => new Promise((resolve) => setTimeout(resolve, 5));
for (let i = 0; i < 20; i++) await tick();  // the page's start-up wires the backdrops

// A press (mousedown) on `down` and a click on `up`, as the browser reports them: each on the
// element under the pointer, bubbling up through the dialog to its backdrop.
function pressAndRelease(down, up) {
  down.dispatchEvent(new w.MouseEvent('mousedown', { bubbles: true }));
  up.dispatchEvent(new w.MouseEvent('click', { bubbles: true }));
}

// The four combinations of press and release on a dialog, each on a freshly opened one, and a click
// with no press of its own on a dialog opened after a backdrop click closed it. `openDialog()` opens
// the dialog and returns { overlay, inside, isOpen }.
const COMBINATIONS = [
  ['inside', 'backdrop', true, 'a drag from inside to the backdrop leaves it open'],
  ['inside', 'inside', true, 'a click inside leaves it open'],
  ['backdrop', 'inside', true, 'a drag from the backdrop to inside leaves it open'],
  ['backdrop', 'backdrop', false, 'a click on the backdrop closes it'],
];
function checkDialog(name, openDialog) {
  for (const [down, up, staysOpen, what] of COMBINATIONS) {
    const d = openDialog();
    const at = (where) => (where === 'inside' ? d.inside : d.overlay);
    pressAndRelease(at(down), at(up));
    check(d.isOpen() === staysOpen, `${name}: ${what}`);
  }
  const closed = openDialog();
  pressAndRelease(closed.overlay, closed.overlay);
  const again = openDialog();
  again.overlay.dispatchEvent(new w.MouseEvent('click', { bubbles: true }));
  check(again.isOpen(), `${name}: opened again, a click without a press of its own leaves it open`);
}

console.log('- File Manager dialogs');
const overlays = [...doc.querySelectorAll('.modal-overlay')];
check(overlays.length >= 8, `the page has its dialogs (${overlays.length})`);
for (const overlay of overlays) {
  const openDialog = () => {
    overlay.classList.add('open');
    return {
      overlay,
      inside: overlay.querySelector('.modal input[type="text"]') || overlay.querySelector('.modal'),
      isOpen: () => overlay.classList.contains('open'),
    };
  };
  if (overlay.id === 'clashModal') {
    // An upload waits on this dialog's answer: no click beside it closes it.
    const d = openDialog();
    pressAndRelease(overlay, overlay);
    check(d.isOpen(), 'clashModal: a click on the backdrop leaves it open');
  } else {
    checkDialog(overlay.id, openDialog);
  }
  overlay.classList.remove('open');
}
{
  const overlay = doc.getElementById('uploadModal');
  overlay.classList.add('open');
  run('isUploadInProgress = true');
  pressAndRelease(overlay, overlay);
  check(overlay.classList.contains('open'), 'uploadModal: during an upload a click on the backdrop leaves it open');
  run('isUploadInProgress = false');
  overlay.classList.remove('open');
}
{
  // The rename field keeps what was typed through a selection drag that ends beside the dialog.
  w.openRenameModal('Old.epub', '/Old.epub', false);
  const input = doc.getElementById('renameNewName');
  input.value = 'New name.epub';
  pressAndRelease(input, doc.getElementById('renameModal'));
  check(doc.getElementById('renameModal').classList.contains('open') && input.value === 'New name.epub',
    'Rename: a selection drag out of the field keeps the dialog and the typed name');
  w.closeRenameModal();
}

console.log('- Details dialog');
run(fs.readFileSync(path.join(HTML_DIR, 'js', 'metadata-editor.js'), 'utf8'));
{
  // Built on each opening and removed again when it closes.
  const openDialog = () => {
    w.MetadataEditor.open('/books/Book.epub', () => {});
    const overlay = doc.querySelector('body > .modal-overlay:last-child');
    return { overlay, inside: overlay && overlay.querySelector('.modal'), isOpen: () => overlay.isConnected };
  };
  const first = openDialog();
  if (check(!!first.overlay && first.overlay.querySelector('#md-close') !== null, 'the Details dialog opens')) {
    checkDialog('Details', openDialog);
  }
}

dom.window.close();
console.log(failures ? `${failures} check(s) FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
