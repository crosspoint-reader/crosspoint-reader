'use strict';

// Real Chromium keyboard-focus regression, with no npm dependencies.
// Run: node --test test/test_webui_plugin_navigation_browser.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const test = require('node:test');

const root = path.resolve(__dirname, '..');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));

function findBrowser() {
  return [
    process.env.WEBUI_TEST_BROWSER,
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    '/usr/bin/chromium', '/usr/bin/chromium-browser',
    '/usr/bin/google-chrome', '/usr/bin/microsoft-edge'
  ].find(candidate => candidate && fs.existsSync(candidate));
}

function fixtureHtml() {
  const page = fs.readFileSync(path.join(root, 'src/network/html/PluginsPage.html'), 'utf8');
  const inline = page.match(/<script>([\s\S]*?)<\/script>/);
  assert.ok(inline, 'PluginsPage.html must contain its navigation script');
  // Exercise the real inline navigation code; only network discovery is stubbed.
  return [
    '<!doctype html><html><head><meta charset="utf-8">',
    '<style>body{margin:0}.card{height:600px}#plugin-picker{position:sticky;top:0}</style>',
    '</head><body><button id="before-action">Before cards</button>',
    '<div id="plugin-picker" hidden><select id="plugin-select">',
    '<option value="">Choose plugin</option></select></div>',
    '<div id="plugin-container">',
    '<section class="card" id="plugin-first"><button id="first-action">First</button></section>',
    '<section class="card" id="plugin-middle"><button id="middle-action">Middle</button></section>',
    '<section class="card" id="plugin-last"><button id="last-action">Last</button></section>',
    '</div><div id="empty-state" hidden><p></p></div>',
    '<p id="plugin-message"></p>',
    '<script>',
    'window.__focusArgs = [];',
    'const nativeFocus = HTMLElement.prototype.focus;',
    'HTMLElement.prototype.focus = function(options) {',
    ' if (this.id === "plugin-last") window.__focusArgs.push(options);',
    ' return nativeFocus.call(this, options);',
    '};',
    'window.loadPlugins = async (mount, preload, discovered) => {',
    ' const plugins = ["first","middle","last"].map(name => ({name, title:name}));',
    ' if (typeof discovered === "function") discovered(plugins);',
    ' else discovered.onDiscovered(plugins, true);',
    ' return true;',
    '};',
    '</script><script>', inline[1], '</script></body></html>'
  ].join('');
}

class CDP {
  constructor(socket) {
    this.socket = socket;
    this.id = 0;
    this.pending = new Map();
    socket.addEventListener('message', event => {
      const data = JSON.parse(event.data);
      if (!data.id || !this.pending.has(data.id)) return;
      const {resolve, reject} = this.pending.get(data.id);
      this.pending.delete(data.id);
      if (data.error) reject(new Error(JSON.stringify(data.error)));
      else resolve(data.result);
    });
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, {resolve, reject});
      this.socket.send(JSON.stringify({id, method, params}));
    });
  }
  async evaluate(expression) {
    const data = await this.send('Runtime.evaluate',
      {expression, returnByValue: true, awaitPromise: true});
    if (data.exceptionDetails) throw new Error(JSON.stringify(data.exceptionDetails));
    return data.result.value;
  }
  close() { this.socket.close(); }
}

async function connect(port) {
  const response = await fetch('http://127.0.0.1:' + port + '/json/list');
  const targets = await response.json();
  const target = targets.find(item => item.type === 'page');
  assert.ok(target, 'browser page target missing');
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, {once: true});
    socket.addEventListener('error', reject, {once: true});
  });
  return new CDP(socket);
}

test('Tab after jumping to a later plugin begins inside its card', {
  timeout: 20000
}, async t => {
  const browser = findBrowser();
  if (!browser) {
    t.skip('Chrome/Edge unavailable; use WEBUI_TEST_BROWSER');
    return;
  }
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'crosspoint-webui-'));
  const child = spawn(browser, [
    '--headless=new', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-extensions',
    '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'
  ], {stdio: 'ignore', windowsHide: true});
  let cdp;
  try {
    let port;
    for (let i = 0; i < 100; i++) {
      const file = path.join(profile, 'DevToolsActivePort');
      if (fs.existsSync(file)) {
        port = Number(fs.readFileSync(file, 'utf8').split('\n')[0]);
        break;
      }
      if (child.exitCode !== null) throw new Error('Headless browser exited before opening');
      await wait(100);
    }
    assert.ok(port, 'headless browser failed to open debugging port');
    cdp = await connect(port);
    await cdp.send('Page.enable');
    await cdp.send('Runtime.enable');
    await cdp.send('Page.navigate',
      {url: 'data:text/html;charset=utf-8,' + encodeURIComponent(fixtureHtml())});
    let ready = false;
    for (let i = 0; i < 100; i++) {
      ready = await cdp.evaluate("document.querySelectorAll('#plugin-select option').length === 4");
      if (ready) break;
      await wait(50);
    }
    assert.ok(ready, 'plugin discovery did not fill the selector');
    const focused = await cdp.evaluate([
      '(function() {',
      ' const picker = document.getElementById("plugin-select");',
      ' picker.focus(); picker.value = "plugin-last";',
      ' picker.dispatchEvent(new Event("change", {bubbles:true}));',
      ' return {id:document.activeElement.id,',
      '  tabIndex:document.getElementById("plugin-last").tabIndex,',
      '  preventScroll:window.__focusArgs[0]?.preventScroll};',
      '})()'
    ].join(''));
    assert.equal(focused.id, 'plugin-last');
    assert.equal(focused.tabIndex, -1);
    assert.equal(focused.preventScroll, true);

    await cdp.send('Input.dispatchKeyEvent', {
      type: 'rawKeyDown', key: 'Tab', code: 'Tab',
      windowsVirtualKeyCode: 9, nativeVirtualKeyCode: 9
    });
    await cdp.send('Input.dispatchKeyEvent', {
      type: 'keyUp', key: 'Tab', code: 'Tab',
      windowsVirtualKeyCode: 9, nativeVirtualKeyCode: 9
    });
    assert.equal(await cdp.evaluate('document.activeElement.id'), 'last-action');
  } finally {
    if (cdp) cdp.close();
    child.kill();
    try { fs.rmSync(profile, {recursive: true, force: true}); } catch {}
  }
});
