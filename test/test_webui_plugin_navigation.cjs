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
    this.value = '';
    this.hidden = false;
    this.listeners = {};
    this.scrollCalls = [];
    this.focusCalls = [];
  }
  appendChild(child) { this.children.push(child); return child; }
  addEventListener(type, callback) { this.listeners[type] = callback; }
  scrollIntoView(options) { this.scrollCalls.push(options); }
  focus(options) { this.focusCalls.push(options); }
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

async function runPluginsPage(plugins) {
  const html = fs.readFileSync(path.join(root, 'src/network/html/PluginsPage.html'), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const host = new Element('div');
  host.children = plugins.map(plugin => {
    const card = new Element('div');
    card.id = 'plugin-' + plugin.name;
    return card;
  });
  const picker = new Element('div');
  picker.hidden = true;
  const select = new Element('select');
  const emptyState = new Element('div');
  emptyState.hidden = true;
  const message = new Element('p');
  const elements = {
    'plugin-container': host,
    'plugin-picker': picker,
    'plugin-select': select,
    'empty-state': emptyState
  };
  let onDiscovered;
  const document = {
    getElementById: id => elements[id] || host.children.find(child => child.id === id) || null,
    querySelector: selector => selector === '#empty-state p' ? message : null,
    createElement: tag => new Element(tag)
  };
  const context = {
    document,
    loadPlugins: async (mount, beforeLoad, callback) => {
      assert.equal(mount, 'settings');
      assert.equal(beforeLoad, null);
      onDiscovered = callback;
      callback(plugins);
      return true;
    }
  };
  vm.createContext(context);
  vm.runInContext(script, context, { filename: 'PluginsPage.html' });
  await new Promise(resolve => setImmediate(resolve));
  return {host, picker, select, emptyState, onDiscovered};
}

test('plugin list handler preserves the 64 KiB manifest read limit', () => {
  const source = fs.readFileSync(path.join(root, 'src/network/CrossPointWebServer.cpp'), 'utf8');
  const handler = source.split('void CrossPointWebServer::handlePluginList() const {')[1]
    .split('// GET /plugin?name=')[0];
  const limit = handler.match(/strcmp\(filename, "manifest\.json"\) == 0 \? (\d+) \* 1024 : \d+/);
  assert.ok(limit, 'manifest.json should retain its separate read allowance');
  assert.equal(Number(limit[1]) * 1024, 64 * 1024);
  assert.match(handler, /Storage\.readFileToString\("WEB", e\.dir \+ "\/" \+ filename, maxBytes, raw\)/);
});

test('Plugins page dropdown navigates to every discovered plugin card', async () => {
  const {host, picker, select, onDiscovered} = await runPluginsPage(installed.slice());
  assert.equal(typeof onDiscovered, 'function');
  assert.equal(picker.hidden, false);
  assert.equal(select.children.length, host.children.length);
  for (let i = 0; i < host.children.length; i++) {
    const card = host.children[i];
    const option = select.children[i];
    assert.equal(option.value, card.id);
    select.value = option.value;
    select.listeners.change();
    assert.equal(card.scrollCalls.length, 1);
    assert.equal(card.scrollCalls[0].behavior, 'smooth');
    assert.equal(card.scrollCalls[0].block, 'start');
    assert.equal(card.tabIndex, -1);
    assert.equal(card.focusCalls.length, 1);
    assert.equal(card.focusCalls[0].preventScroll, true);
  }
});

test('Plugins page hides jump picker when discovery returns no plugins', async () => {
  const {picker, select, emptyState} = await runPluginsPage([]);
  assert.equal(picker.hidden, true);
  assert.equal(select.children.length, 0);
  assert.equal(emptyState.hidden, false);
});
