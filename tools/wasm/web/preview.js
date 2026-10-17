import { createFileManager } from './file-manager.js';
import { tr, locale } from './strings.js';
import { devices, keyMap } from './devices.js';
import { embedded, messageOrigin, targetOrigin } from './assets.js';

document.documentElement.lang = locale;
document.title = `CrossPoint · ${tr('preview')}`;
for (const element of document.querySelectorAll('[data-i18n]')) element.textContent = tr(element.dataset.i18n);
const $ = (selector) => document.querySelector(selector);
const deviceSelect = $('#device-select');
const params = new URLSearchParams(location.search);
deviceSelect.value = embedded?.device ?? (Object.hasOwn(devices, params.get('device')) ? params.get('device') : 'x4');
if (embedded) {
  for (const option of [...deviceSelect.options]) if (option.value !== embedded.device) option.remove();
  deviceSelect.disabled = true;
}
let frame;
let cardSnapshot = null;
let requestId = 0;
const pending = new Map();
let logLines = [];
let buildInfo = {};
let generation = 0;
const held = new Set();

function status(key, state = '') {
  $('#status').textContent = tr(key);
  $('#status-dot').className = state;
}
function send(type, extra = {}) { frame?.contentWindow.postMessage({ type, ...extra }, targetOrigin); }
function release() {
  held.clear();
  document.querySelectorAll('.held').forEach((button) => button.classList.remove('held'));
  send('release');
}
function log(text) {
  logLines.push(String(text).slice(0, 4000));
  if (logLines.length > 500) logLines.shift();
  $('#log-output').textContent = logLines.join('\n');
}
function download(blob, name) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function sdRequest(payload) {
  return new Promise((resolve, reject) => {
    if (!frame) { reject({ code: 'notReady' }); return; }
    const id = ++requestId;
    const timer = setTimeout(() => { pending.delete(id); reject({ code: 'fileError' }); }, 30000);
    pending.set(id, { resolve, reject, timer });
    send('sd-request', { id, ...payload });
  });
}
const fileManager = createFileManager(sdRequest, download, release, restartFirmware);

async function restartFirmware() {
  const current = generation;
  const previous = buildInfo.device;
  const running = frame && !$('#screenshot').disabled;
  fileManager.reset();
  deviceSelect.disabled = true;
  release();
  try {
    const snapshot = running ? (await sdRequest({ operation: 'snapshot' })).entries : null;
    if (current !== generation) return;
    cardSnapshot = snapshot;
    $('#sd-dialog').close();
    await restart();
  } catch (error) {
    if (current !== generation) return;
    deviceSelect.value = previous;
    if (running) fileManager.ready();
    log(error);
    status(error.code || 'fileError', 'error');
  }
}

async function restart() {
  const current = ++generation;
  fileManager.reset();
  for (const request of pending.values()) { clearTimeout(request.timer); request.reject({ code: 'notReady' }); }
  pending.clear();
  release();
  frame?.contentWindow.disposePreview?.();
  frame?.remove();
  frame = null;
  logLines = [];
  $('#log-output').textContent = '';
  $('#screenshot').disabled = true;
  status('loading');
  const device = deviceSelect.value;
  const profile = devices[device];
  $('#device-name').textContent = profile.name;
  $('#input-help').textContent = tr(profile.touch ? 'touchHelp' : 'keyboard');
  for (const button of document.querySelectorAll('[data-button]')) {
    button.hidden = !profile.buttons.includes(Number(button.dataset.button));
  }
  if (!embedded) history.replaceState(null, '', `?device=${device}`);
  try {
    let info = embedded?.info;
    if (!info) {
      const response = await fetch(`./${device}/build.json`);
      if (!response.ok) throw new Error(tr('missing'));
      info = await response.json();
    }
    if (current !== generation) return;
    buildInfo = info;
    $('#revision').textContent = info.sha.slice(0, 7) + (info.dirty ? ` · ${tr('local')}` : '');
    log(JSON.stringify(info));
    frame = document.createElement('iframe');
    frame.title = tr('frameTitle');
    if (embedded) frame.srcdoc = embedded.frame;
    else frame.src = './frame.html';
    $('#screen').replaceChildren(frame);
  } catch (error) {
    if (current !== generation) return;
    log(error);
    status('error', 'error');
  }
}

window.addEventListener('message', async (event) => {
  if (!frame || event.source !== frame.contentWindow || event.origin !== messageOrigin) return;
  const data = event.data;
  if (data.type === 'frame-ready') {
    const entries = cardSnapshot;
    cardSnapshot = null;
    frame.contentWindow.postMessage({ type: 'init', device: deviceSelect.value, entries }, targetOrigin,
      entries?.filter(entry => entry.bytes).map(entry => entry.bytes) ?? []);
  } else if (data.type === 'booting') status('booting');
  else if (data.type === 'running') { status('ready', 'ready'); $('#screenshot').disabled = false; fileManager.ready(); }
  else if (data.type === 'failed') status('error', 'error');
  else if (data.type === 'timeout') status('timeout', 'error');
  else if (data.type === 'sd-response') {
    const request = pending.get(data.id);
    if (!request) return;
    pending.delete(data.id);
    clearTimeout(request.timer);
    if (data.error) request.reject(data.error); else request.resolve(data.result);
  }
  else if (data.type === 'log') log(data.text);
  else if (data.type === 'geometry') {
    $('#screen').style.aspectRatio = `${data.width} / ${data.height}`;
    $('#device').classList.toggle('landscape', data.width > data.height);
  } else if (data.type === 'screenshot' && data.blob instanceof Blob) {
    download(data.blob, `crosspoint-${deviceSelect.value}-${buildInfo.sha.slice(0, 7)}.png`);
  }
});

for (const button of document.querySelectorAll('[data-button]')) {
  button.addEventListener('pointerdown', (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    button.classList.add('held');
    send('button', { button: Number(button.dataset.button), down: 1 });
  });
  const lift = () => {
    if (!button.classList.contains('held')) return;
    button.classList.remove('held');
    send('button', { button: Number(button.dataset.button), down: 0 });
  };
  button.addEventListener('pointerup', lift);
  button.addEventListener('pointercancel', lift);
  button.addEventListener('lostpointercapture', lift);
  button.addEventListener('click', (event) => {
    if (event.detail !== 0) return;
    send('button', { button: Number(button.dataset.button), down: 1 });
    setTimeout(() => send('button', { button: Number(button.dataset.button), down: 0 }), 100);
  });
}
window.addEventListener('keydown', (event) => {
  if (fileManager.isOpen || event.target.closest('button, input, select, summary, a')) return;
  const button = keyMap[event.key];
  if (!devices[deviceSelect.value].buttons.includes(button) || event.repeat) return;
  event.preventDefault();
  held.add(button);
  send('button', { button, down: 1 });
});
window.addEventListener('keyup', (event) => {
  const button = keyMap[event.key];
  if (!held.delete(button)) return;
  event.preventDefault();
  send('button', { button, down: 0 });
});
window.addEventListener('blur', release);
document.addEventListener('visibilitychange', () => { if (document.hidden) release(); });

deviceSelect.addEventListener('change', restartFirmware);
$('#restart').addEventListener('click', restartFirmware);
$('#reset').addEventListener('click', () => {
  if (!window.confirm(tr('resetConfirm'))) return;
  cardSnapshot = null;
  restart();
});
$('#screenshot').addEventListener('click', () => send('screenshot'));
$('#logs').addEventListener('click', () => download(new Blob([JSON.stringify(buildInfo, null, 2), '\n', logLines.join('\n')], { type: 'text/plain' }), 'crosspoint-preview.log'));
$('#copy').addEventListener('click', async () => {
  try { await navigator.clipboard.writeText(JSON.stringify(buildInfo, null, 2)); status('copied', 'ready'); }
  catch { status('copyFailed', 'error'); }
});
restart();
