'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const hostScript = fs.readFileSync(path.join(root, 'src/network/html/js/plugin-host.js'), 'utf8');

class Element {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.id = '';
    this._text = '';
    this.style = {};
  }
  appendChild(child) { this.children.push(child); return child; }
  replaceChildren(...children) { this.children = children; this._text = ''; }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text || this.children.map(child => child.textContent).join(' '); }
}

async function load(mount, plugins, {fail = [], onDiscovered} = {}) {
  const host = new Element('div');
  const requests = [];
  const document = {
    getElementById: id => id === 'plugin-container' ? host : null,
    createElement: tag => new Element(tag),
    body: {
      appendChild(script) {
        requests.push(script.src);
        const name = new URL(script.src, 'http://device.test').searchParams.get('name');
        queueMicrotask(() => {
          if (fail.includes(name)) return script.onerror();
          script.cpRender = container => { container.textContent = 'Rendered ' + name; };
          script.onload();
        });
      }
    }
  };
  const context = { document, console, setTimeout, clearTimeout, encodeURIComponent,
    fetch: async () => ({ json: async () => plugins }) };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(hostScript, context, { filename: 'plugin-host.js' });
  const result = await context.loadPlugins(mount, null, onDiscovered && (list => onDiscovered(list, host, requests)));
  return { result, host, requests };
}

const installed = [
  { name: 'zeta', title: 'Zeta', hasPluginJs: true, mount: 'settings' },
  { name: 'native', title: 'Native', version: '1.2', author: 'Reader', hasPluginJs: false },
  { name: 'pluginhub', title: 'Plugin Hub', hasPluginJs: true, mount: 'settings' },
  { name: 'transfer', title: 'Transfer', hasPluginJs: true, mount: 'files' }
];

test('settings page lists all installed plugins and provides anchors before script loads', async () => {
  let discovered = false;
  const {result, host, requests} = await load('settings', installed.slice(), {
    onDiscovered(plugins, cards, pending) {
      discovered = true;
      assert.equal(plugins.length, 4);
      assert.equal(cards.children.length, 4);
      assert.equal(cards.children[0].id, 'plugin-pluginhub');
      assert.equal(pending.length, 0);
      assert.match(cards.children[1].textContent, /Native/);
      assert.match(cards.children[2].textContent, /File Manager page/);
    }
  });
  assert.equal(discovered, true);
  assert.equal(result, true);
  assert.equal(requests.length, 2);
  assert.deepEqual(host.children.map(e => e.id),
    ['plugin-pluginhub', 'plugin-native', 'plugin-transfer', 'plugin-zeta']);
  assert.match(host.children[3].textContent, /Rendered zeta/);
});

test('File Manager and job runner still skip native-only plugins', async () => {
  const files = await load('files', installed.slice());
  assert.equal(files.result, true);
  assert.deepEqual(files.host.children.map(e => e.id), ['plugin-transfer']);
  assert.equal(files.requests.length, 1);

  const runner = await load(null, installed.slice());
  assert.equal(runner.result, true);
  assert.equal(runner.requests.length, 3);
  assert.equal(runner.host.children.some(e => e.id === 'plugin-native'), false);
});

test('a failed script keeps the plugin anchor and displays a failure', async () => {
  const {host, result} = await load('settings',
    [{name: 'broken', hasPluginJs: true}], {fail: ['broken']});
  assert.equal(result, true);
  assert.equal(host.children[0].id, 'plugin-broken');
  assert.match(host.children[0].textContent, /Failed to load plugin/);
});

test('a malformed plugin list is rejected without creating cards', async () => {
  const {result, host} = await load('settings', {error: 'out of memory'});
  assert.equal(result, false);
  assert.equal(host.children.length, 0);
});

test('Plugins page keeps the dropdown and scroll navigation wired', () => {
  const html = fs.readFileSync(path.join(root, 'src/network/html/PluginsPage.html'), 'utf8');
  assert.match(html, /id="plugin-select"/);
  assert.match(html, /loadPlugins\('settings', null, onDiscovered\)/);
  assert.match(html, /scrollIntoView\(\{behavior: 'smooth', block: 'start'\}\)/);
});
