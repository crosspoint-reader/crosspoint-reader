import { tr } from './strings.js';
import { SD_LIMIT } from './sd-card.js';

const $ = selector => document.querySelector(selector);
const join = (folder, name) => `${folder === '/' ? '' : folder}/${name}`;
const nameOf = path => path.slice(path.lastIndexOf('/') + 1);
const sizeOf = bytes => `${(bytes / 1024 / 1024).toFixed(1)} MiB`;

export function createFileManager(request, download, release, restartFirmware) {
  let path = '/books';
  let ready = false;
  let busy = false;
  let epoch = 0;
  let listing = [];
  let selected = null;
  const dialog = $('#sd-dialog');
  const message = (key, detail = '') => { $('#sd-message').textContent = `${tr(key)}${detail ? ` ${detail}` : ''}`; };
  function controls() {
    $('#open-sd').disabled = !ready;
    $('#restart').disabled = !ready || busy;
    for (const element of dialog.querySelectorAll('button, input')) element.disabled = !ready || busy;
    $('#sd-close').disabled = false;
    for (const id of ['rename', 'delete', 'download']) $(`#sd-${id}`).disabled = !ready || busy || !selected || (id === 'download' && selected.directory);
    $('#device-select').disabled = busy || $('#device-select').options.length === 1;
  }
  function render() {
    const parts = path.split('/').filter(Boolean);
    const crumbs = $('#sd-path');
    crumbs.replaceChildren();
    for (let i = 0; i <= parts.length; i++) {
      const button = document.createElement('button');
      button.textContent = i === 0 ? tr('sdRoot') : parts[i - 1];
      button.addEventListener('click', () => run(() => refresh('/' + parts.slice(0, i).join('/'))));
      crumbs.append(button);
    }
    $('#sd-location').textContent = path;
    const showHidden = $('#sd-hidden').checked;
    $('#sd-list').replaceChildren(...listing.filter(entry => showHidden || !nameOf(entry.path).startsWith('.')).map(entry => {
      const row = document.createElement('tr');
      row.dataset.path = entry.path;
      row.classList.toggle('selected', selected?.path === entry.path);
      const name = document.createElement('td');
      const button = document.createElement('button');
      button.className = 'file-name';
      button.textContent = nameOf(entry.path) + (entry.directory ? '/' : '');
      button.setAttribute('aria-pressed', String(selected?.path === entry.path));
      button.addEventListener('click', () => {
        selected = entry;
        for (const row of $('#sd-list').children) {
          const active = row.dataset.path === entry.path;
          row.classList.toggle('selected', active);
          row.querySelector('.file-name').setAttribute('aria-pressed', String(active));
        }
        controls();
      });
      button.addEventListener('dblclick', () => { if (entry.directory) run(() => refresh(entry.path)); });
      name.append(button);
      const size = document.createElement('td');
      size.textContent = entry.directory ? tr('folder') : entry.size < 1024 ? `${entry.size} B` : entry.size < 1024 * 1024 ? `${(entry.size / 1024).toFixed(1)} KiB` : sizeOf(entry.size);
      const action = document.createElement('td');
      if (entry.directory) {
        const open = document.createElement('button');
        open.textContent = tr('openFolder');
        open.setAttribute('aria-label', `${tr('openFolder')}: ${nameOf(entry.path)}`);
        open.addEventListener('click', () => run(() => refresh(entry.path)));
        action.append(open);
      }
      row.append(name, size, action);
      return row;
    }));
    $('#sd-empty').hidden = $('#sd-list').children.length > 0;
    controls();
  }
  async function refresh(next = path) {
    const current = epoch;
    const data = await request({ operation: 'list', path: next });
    if (current !== epoch) return;
    path = next;
    listing = data.entries.sort((a, b) => Number(b.directory) - Number(a.directory) || nameOf(a.path).localeCompare(nameOf(b.path)));
    selected = null;
    $('#sd-usage').textContent = `${sizeOf(data.used)} / ${sizeOf(data.limit)}`;
    render();
  }
  async function run(action) {
    if (!ready || busy) return;
    busy = true;
    controls();
    const current = epoch;
    message('working');
    try { await action(current); if (current === epoch) message('sdReady'); }
    catch (error) { if (current === epoch) message(error.code || 'fileError', error.path || ''); }
    finally { if (current === epoch) { busy = false; controls(); } }
  }
  async function upload(items, current) {
    const folder = path;
    let completed = 0;
    for (const item of items) {
      if (current !== epoch) return;
      const destination = join(folder, item.path);
      if (item.directory) {
        try { await request({ operation: 'mkdir', path: destination }); }
        catch (error) { if (error.code !== 'alreadyExists') throw error; }
      } else {
        if (item.file.size > SD_LIMIT) throw { code: 'storageFull', path: destination };
        message('uploading', `${completed + 1}/${items.length}: ${item.path}`);
        const bytes = await item.file.arrayBuffer();
        if (current !== epoch) return;
        // Keep this buffer until a possible overwrite confirmation has completed.
        try { await request({ operation: 'write', path: destination, bytes }); }
        catch (error) {
          if (error.code !== 'alreadyExists' || !window.confirm(`${tr('replaceFile')}\n${destination}`)) throw error;
          await request({ operation: 'write', path: destination, bytes, replace: true });
        }
      }
      completed++;
    }
    await refresh(folder);
  }
  async function importFiles(files, current) {
    if (files.length > 8192) throw { code: 'tooManyFiles' };
    const items = Array.from(files, file => ({ path: file.webkitRelativePath || file.name, file }));
    try { await upload(items, current); }
    finally { if (current === epoch) await refresh(); }
  }
  $('#open-sd').addEventListener('click', () => { release(); dialog.showModal(); run(() => refresh()); });
  $('#sd-close').addEventListener('click', () => dialog.close());
  $('#sd-hidden').addEventListener('change', () => { selected = null; render(); });
  $('#sd-refresh').addEventListener('click', () => run(() => refresh()));
  $('#sd-restart').addEventListener('click', () => run(restartFirmware));
  for (const id of ['file-input', 'folder-input']) {
    $(`#${id}`).addEventListener('change', event => {
      const files = [...event.target.files];
      event.target.value = '';
      run(current => importFiles(files, current));
    });
  }
  $('#sd-new-folder').addEventListener('click', () => run(async () => {
    const name = window.prompt(tr('folderName'));
    if (!name) return;
    if (/[\/\\]/.test(name)) throw { code: 'invalidPath', path: name };
    await request({ operation: 'mkdir', path: join(path, name) });
    await refresh();
  }));
  $('#sd-rename').addEventListener('click', () => run(async () => {
    const name = window.prompt(tr('newName'), nameOf(selected.path));
    if (!name || name === nameOf(selected.path)) return;
    if (/[\/\\]/.test(name)) throw { code: 'invalidPath', path: name };
    await request({ operation: 'rename', path: selected.path, destination: join(path, name) });
    await refresh();
  }));
  $('#sd-delete').addEventListener('click', () => run(async () => {
    if (!window.confirm(`${tr('deleteFile')}\n${selected.path}`)) return;
    await request({ operation: 'delete', path: selected.path });
    await refresh();
  }));
  $('#sd-download').addEventListener('click', () => run(async () => {
    const name = nameOf(selected.path);
    const result = await request({ operation: 'download', path: selected.path });
    download(new Blob([result.bytes]), name);
  }));
  const dropZone = $('#sd-drop-zone');
  dropZone.addEventListener('dragover', event => { event.preventDefault(); dropZone.classList.add('dragging'); });
  dropZone.addEventListener('dragleave', () => dropZone.classList.remove('dragging'));
  dropZone.addEventListener('drop', event => {
    event.preventDefault();
    dropZone.classList.remove('dragging');
    // Capture entries while DataTransfer is still readable, before the first await.
    const entries = [...event.dataTransfer.items].map(item => item.webkitGetAsEntry?.()).filter(Boolean);
    const files = [...event.dataTransfer.files];
    run(async current => {
      if (!entries.length) return importFiles(files, current);
      const items = [];
      async function visit(entry, prefix = '', depth = 0) {
        if (depth > 32 || items.length >= 8192) throw { code: 'tooManyFiles' };
        const path = prefix + entry.name;
        if (entry.isFile) items.push({ path, file: await new Promise((resolve, reject) => entry.file(resolve, reject)) });
        else {
          items.push({ path, directory: true });
          const reader = entry.createReader();
          while (true) {
            const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
            if (!batch.length) break;
            for (const child of batch) await visit(child, path + '/', depth + 1);
          }
        }
      }
      for (const entry of entries) await visit(entry);
      try { await upload(items, current); }
      finally { if (current === epoch) await refresh(); }
    });
  });
  controls();
  return {
    reset() { epoch++; ready = false; busy = false; path = '/books'; listing = []; selected = null; render(); message('loading'); },
    ready() { ready = true; controls(); if (dialog.open) run(() => refresh()); },
    get isOpen() { return dialog.open; },
  };
}
