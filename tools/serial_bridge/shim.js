/* CrossPoint serial file manager shim.
 *
 * Runs before the stock CrossPoint web UI scripts and replaces the network
 * layer (fetch, XMLHttpRequest, WebSocket, download links) with the framed
 * serial protocol of the reader's "Serial Transfer" mode (docs/serial-transfer.md),
 * using the browser's Web Serial API (Chrome / Edge). The page asks the user to
 * pick the serial port; nothing is installed on the computer.
 */
(() => {
  'use strict';
  const BAUD_FAST = 921600;
  const BAUD_SLOW = 115200;
  const CHUNK = 4000;
  const CMD = { HELLO: 1, LIST: 2, READ_OPEN: 3, READ: 4, CLOSE: 5, WRITE_OPEN: 6, WRITE: 7, WRITE_END: 8,
                MKDIR: 9, RENAME: 10, DELETE: 11, BAUD: 12, STATUS: 13, API: 14, API_READ: 15 };
  const STATUS_TEXT = { 1: 'unknown command', 2: 'bad argument', 3: 'not found',
                        4: 'already exists (or folder not empty)', 5: 'protected path', 6: 'I/O error', 7: 'bad state' };
  const enc = new TextEncoder();
  const dec = new TextDecoder();

  class DeviceError extends Error {
    constructor(status, what, detail) {
      super((what ? what + ': ' : '') + (STATUS_TEXT[status] || 'error ' + status) +
            (detail ? ' (original file kept as ' + detail + ')' : ''));
      this.status = status;
    }
  }

  // ---- CRC32 (zlib) ----
  const crcTable = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    crcTable[i] = c >>> 0;
  }
  function crc32(...parts) {
    let c = 0xFFFFFFFF;
    for (const p of parts) for (let i = 0; i < p.length; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  const u32 = (v) => new Uint8Array([v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
  const cat = (...a) => { const n = a.reduce((s, x) => s + x.length, 0); const o = new Uint8Array(n); let p = 0;
                          for (const x of a) { o.set(x, p); p += x.length; } return o; };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---- serial link ----
  const link = {
    received: 0, port: null, reader: null, writer: null, rx: new Uint8Array(0), waiter: null, reading: false, baud: 0,
    chain: Promise.resolve(), ops: Promise.resolve(), nextId: 0,

    async open(port, baud) {
      await port.open({ baudRate: baud, bufferSize: 1 << 16 });
      // DTR on, RTS off: the line state verified to work with the reader (both off froze it).
      try { await port.setSignals({ dataTerminalReady: true, requestToSend: false }); } catch (e) { /* optional */ }
      this.port = port; this.baud = baud; this.rx = new Uint8Array(0);
      this.reader = port.readable.getReader(); this.writer = port.writable.getWriter();
      this.reading = true;
      (async () => {
        try {
          while (this.reading) {
            const { value, done } = await this.reader.read();
            if (done) break;
            if (value && value.length) { this.received += value.length; this.rx = cat(this.rx, value); if (this.waiter) this.waiter(); }
          }
        } catch (e) { /* closed */ }
      })();
    },
    async close() {
      this.reading = false;
      try { await this.reader.cancel(); } catch (e) { /* */ }
      try { this.reader.releaseLock(); } catch (e) { /* */ }
      try { this.writer.releaseLock(); } catch (e) { /* */ }
      try { await this.port.close(); } catch (e) { /* */ }
    },
    // Waits until the reply to request `id` is buffered, scanning for the sync bytes.
    // Replies carrying another id (late answers to an earlier attempt) are dropped.
    async recv(timeout, id) {
      const deadline = Date.now() + timeout;
      for (;;) {
        const b = this.rx;
        let i = 0;
        while (i + 1 < b.length && !(b[i] === 0xA5 && b[i + 1] === 0x5A)) i++;
        if (i + 1 >= b.length) { this.rx = b.slice(Math.max(0, b.length - 1)); }
        else if (i + 6 <= b.length) {
          // Reply layout: A5 5A | status | id | len(u16 LE) | payload | crc32
          const len = b[i + 4] | (b[i + 5] << 8);
          if (len > 4096) { this.rx = b.slice(i + 2); continue; }
          if (i + 6 + len + 4 <= b.length) {
            const head = b.slice(i + 2, i + 6), payload = b.slice(i + 6, i + 6 + len);
            const crc = (b[i + 6 + len] | (b[i + 7 + len] << 8) | (b[i + 8 + len] << 16) | (b[i + 9 + len] << 24)) >>> 0;
            this.rx = b.slice(i + 10 + len);
            if (crc === crc32(head, payload) && head[1] === id) return { status: head[0], payload };
            continue;  // corrupt or for another request: keep scanning
          }
        }
        const left = deadline - Date.now();
        if (left <= 0) throw new Error('timeout');
        await new Promise((res) => { const t = setTimeout(res, left); this.waiter = () => { clearTimeout(t); res(); }; });
        this.waiter = null;
      }
    },
    call(cmd, payload = new Uint8Array(0), opt = {}) {
      const run = async () => {
        const timeout = opt.timeout || 3000, retries = opt.retries || 3;
        // One id per request, reused by its retries so a late reply to an earlier attempt still matches.
        this.nextId = (this.nextId % 255) + 1;
        const id = this.nextId;
        const head = new Uint8Array([cmd, id, payload.length & 255, payload.length >> 8]);
        const frame = cat(new Uint8Array([0xA5, 0x5A]), head, payload, u32(crc32(head, payload)));
        let last;
        for (let a = 0; a < retries; a++) {
          try {
            await this.writer.write(frame);
            const r = await this.recv(timeout, id);
            if (r.status !== 0) throw new DeviceError(r.status, opt.what, r.payload.length ? dec.decode(r.payload) : '');
            return r.payload;
          } catch (e) {
            if (e instanceof DeviceError) throw e;
            last = e;
          }
        }
        throw new Error('The reader did not answer (' + (last && last.message) + '). Is Serial Transfer open on it?');
      };
      const p = this.chain.then(run, run);
      this.chain = p.catch(() => {});
      return p;
    },
    // Runs a whole multi-frame operation (open/read/close, open/write/commit, API + body reads)
    // without any other operation touching the reader's single file handle or API buffer.
    exclusive(fn) {
      const p = this.ops.then(fn, fn);
      this.ops = p.catch(() => {});
      return p;
    },
  };

  // ---- connection / overlay ----
  let connected = false;
  let resolveConnected;
  const connectedPromise = new Promise((r) => { resolveConnected = r; });

  const style = document.createElement('style');
  style.textContent = `#cpx[hidden],#cpx-badge[hidden],#cpx button[hidden]{display:none!important}
    #cpx{position:fixed;inset:0;z-index:99999;background:rgba(0,0,0,.55);display:flex;
    align-items:center;justify-content:center;font-family:system-ui,sans-serif}
    #cpx .box{background:#fff;color:#111;max-width:30em;margin:1em;padding:1.5em;border-radius:12px;box-shadow:0 8px 40px #0006}
    #cpx button{font-size:1em;padding:.6em 1.2em;border-radius:8px;border:0;background:#2563eb;color:#fff;cursor:pointer}
    #cpx p{margin:.6em 0} #cpx .msg{color:#555;font-size:.9em;min-height:1.2em}
    #cpx-badge{position:fixed;right:8px;bottom:8px;z-index:99998;background:#111c;color:#fff;font:12px system-ui;
    padding:4px 8px;border-radius:6px} #cpx-badge a{color:#9cf;cursor:pointer;margin-left:6px}`;
  const overlay = document.createElement('div');
  overlay.id = 'cpx';
  overlay.innerHTML = `<div class="box"><h2>CrossPoint serial file manager</h2>
    <p id="cpx-text">Connect the reader's USB-C cable, then choose its serial port.</p>
    <p><button id="cpx-btn">Choose serial port</button> <button id="cpx-cancel" hidden>Cancel</button></p><p class="msg" id="cpx-msg"></p></div>`;
  const badge = document.createElement('div');
  badge.id = 'cpx-badge'; badge.hidden = true;
  const transfer = document.createElement('div');
  transfer.id = 'cpx-badge'; transfer.style.bottom = '36px'; transfer.hidden = true;
  const mount = () => { document.head.appendChild(style); document.body.append(overlay, badge, transfer); };
  if (document.body) mount(); else document.addEventListener('DOMContentLoaded', mount);
  const $ = (id) => overlay.querySelector(id);
  const setMsg = (t) => { const m = $('#cpx-msg'); if (m) m.textContent = t || ''; };
  const showProgress = (t) => { transfer.hidden = !t; transfer.textContent = t || ''; };

  let handshakeCancelled = false;
  let connecting = false;  // one owner for the shared link: an automatic or a manual attempt
  const showChooser = (msg) => {
    $('#cpx-text').textContent = "Connect the reader's USB-C cable, then choose its serial port.";
    $('#cpx-btn').hidden = false; $('#cpx-cancel').hidden = true;
    setMsg(msg);
  };
  async function abandon(msg) {
    try { await link.close(); } catch (e) { /* already closed */ }
    showChooser(msg);
  }
  // Tries HELLO until the reader answers, the user cancels or `limitMs` passes.
  async function handshake(timeout, limitMs, progress) {
    const end = limitMs ? Date.now() + limitMs : Infinity;
    for (let attempt = 1; !handshakeCancelled && Date.now() < end; attempt++) {
      try { await link.call(CMD.HELLO, new Uint8Array(0), { timeout, retries: 1 }); return !handshakeCancelled; }
      catch (e) { if (progress) progress(attempt); await sleep(300); }
    }
    return false;
  }

  // Opens the port ONCE at 115200 (the reader's start speed) and keeps retrying HELLO
  // without closing it: re-opening toggles DTR/RTS, which can reset the reader.
  async function connect(port) {
    try { await connectOnce(port); } finally { connecting = false; }
  }
  async function connectOnce(port) {
    handshakeCancelled = false;
    $('#cpx-text').textContent = 'Opening the port... On the reader, open File Transfer > Serial Transfer.';
    $('#cpx-btn').hidden = true; $('#cpx-cancel').hidden = false;
    try { await link.open(port, BAUD_SLOW); } catch (e) { showChooser('Cannot open the port: ' + e.message); return; }
    const first = await handshake(800, 0, (attempt) => setMsg(
      'Waiting for the reader (Serial Transfer must be open on it)... try ' + attempt +
      ' - bytes received from the reader so far: ' + link.received +
      (link.received ? ' (it sends data but not valid frames: baud/pins?)' : ' (nothing at all)')));
    if (!first || handshakeCancelled) { await abandon('Connection cancelled.'); return; }
    // The reader falls back to 115200 by itself if it hears nothing valid at the new speed.
    let fast = false;
    try {
      await link.call(CMD.BAUD, u32(BAUD_FAST));
      await sleep(50);
      await link.close();
      await link.open(port, BAUD_FAST);
      fast = await handshake(800, 3000);
    } catch (e) { /* fall through to the slow speed */ }
    if (handshakeCancelled) { await abandon('Connection cancelled.'); return; }
    if (!fast) {
      setMsg('Fast speed failed, going back to ' + BAUD_SLOW + ' baud...');
      try {
        await link.close();
        await link.open(port, BAUD_SLOW);
        // The reader needs a few seconds to give up on the fast speed.
        if (!(await handshake(1000, 12000))) throw new Error('no answer');
      } catch (e) { await abandon('Lost the reader after the speed change. Choose the port again.'); return; }
    }
    connected = true;
    overlay.hidden = true;
    badge.hidden = false;
    badge.innerHTML = 'Serial ' + link.baud + ' baud <a id="cpx-change">change port</a>';
    badge.querySelector('#cpx-change').onclick = () => location.reload();
    resolveConnected();
    navigate('/files');
  }

  $('#cpx-btn').onclick = async () => {
    if (!('serial' in navigator)) { setMsg('Web Serial is not available. Use Chrome or Edge.'); return; }
    if (connecting) return;
    connecting = true;
    let port;
    try { port = await navigator.serial.requestPort(); } catch (e) { connecting = false; setMsg(e.message); return; }
    connect(port);
  };
  $('#cpx-cancel').onclick = () => { handshakeCancelled = true; };
  if (!('serial' in navigator)) {
    setMsg('This browser has no Web Serial support. Open the page in Chrome or Edge.');
  } else {
    navigator.serial.getPorts().then((ports) => {
      if (ports.length === 1 && !connecting) { connecting = true; connect(ports[0]); }
    }).catch(() => {});
  }

  // ---- file operations ----
  // Paths arrive already decoded (URLSearchParams / form parsing): never decode them again.
  const norm = (p) => '/' + (p || '/').replace(/\\/g, '/').split('/').filter(Boolean).join('/');
  const joinPath = (a, b) => norm(a + '/' + b);
  const parentOf = (p) => norm(p.slice(0, p.lastIndexOf('/')) || '/');

  async function listDir(path) {
    const out = []; let start = 0;
    for (;;) {
      const d = await link.call(CMD.LIST, cat(u32(start), enc.encode(path)), { what: 'list ' + path });
      const more = d[0]; let i = 1;
      while (i < d.length) {
        const flags = d[i], size = (d[i + 1] | (d[i + 2] << 8) | (d[i + 3] << 16) | (d[i + 4] << 24)) >>> 0, n = d[i + 5];
        const name = dec.decode(d.slice(i + 6, i + 6 + n)); i += 6 + n; start++;
        out.push({ name, size, isDirectory: !!(flags & 1), isEpub: !(flags & 1) && /\.epub$/i.test(name) });
      }
      if (!more) return out;
    }
  }
  const readFile = (path, onProgress) => link.exclusive(() => readFileOp(path, onProgress));
  async function readFileOp(path, onProgress) {
    const o = await link.call(CMD.READ_OPEN, enc.encode(path), { what: 'open ' + path });
    const size = (o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24)) >>> 0;
    const parts = []; let off = 0;
    try {
      while (off < size) {
        const n = Math.min(CHUNK, size - off);
        const d = await link.call(CMD.READ, cat(u32(off), new Uint8Array([n & 255, n >> 8])), { what: 'read' });
        if (!d.length) throw new Error('short read');
        parts.push(d); off += d.length;
        if (onProgress) onProgress(off, size);
      }
    } finally { await link.call(CMD.CLOSE, new Uint8Array(0), { retries: 1 }).catch(() => {}); }
    return new Blob(parts);
  }
  const writeFile = (path, data, overwrite, onProgress, signal) =>
    link.exclusive(() => writeFileOp(path, data, overwrite, onProgress, signal || {}));
  async function writeFileOp(path, data, overwrite, onProgress, signal) {
    if (signal.aborted) throw new Error('upload cancelled');
    let opened = false;
    try {
      try {
        await link.call(CMD.WRITE_OPEN, cat(new Uint8Array([overwrite ? 1 : 0]), enc.encode(path)), { what: 'create ' + path });
      } catch (e) {
        // A refusal means no stage exists; a lost reply may hide an open one, so close it.
        if (!(e instanceof DeviceError)) opened = true;
        throw e;
      }
      opened = true;
      for (let off = 0; off < data.length; off += CHUNK) {
        if (signal.aborted) throw new Error('upload cancelled');
        await link.call(CMD.WRITE, cat(u32(off), data.subarray(off, off + CHUNK)), { what: 'write' });
        if (onProgress) onProgress(Math.min(off + CHUNK, data.length), data.length);
      }
      if (signal.aborted) throw new Error('upload cancelled');
      await link.call(CMD.WRITE_END, new Uint8Array(0), { timeout: 15000, what: 'finish ' + path });
    } catch (e) {
      if (opened) await link.call(CMD.CLOSE, new Uint8Array(0), { retries: 1 }).catch(() => {});
      throw e;
    }
  }

  // Settings / fonts are served by the reader's own web-server code (CMD_API).
  const callApi = (method, uri, body) => link.exclusive(() => callApiOp(method, uri, body));
  async function callApiOp(method, uri, body) {
    const m = new Uint8Array([method]);
    const payload = body ? cat(m, enc.encode(uri), new Uint8Array([0]), enc.encode(body)) : cat(m, enc.encode(uri));
    const d = await link.call(CMD.API, payload, { timeout: 20000, what: 'request' });
    const http = d[0] | (d[1] << 8);
    const total = (d[2] | (d[3] << 8) | (d[4] << 16) | (d[5] << 24)) >>> 0;
    const parts = []; let off = 0;
    while (off < total) {
      const n = Math.min(CHUNK, total - off);
      const c = await link.call(CMD.API_READ, cat(u32(off), new Uint8Array([n & 255, n >> 8])), { what: 'read' });
      if (!c.length) throw new Error('The reader returned a short settings/fonts reply');
      parts.push(c); off += c.length;
    }
    return { status: http, text: dec.decode(cat(...parts)) };
  }

  const fieldOf = (body, name) => {
    if (typeof body === 'string') return new URLSearchParams(body).get(name);
    if (body && typeof body.get === 'function') return body.get(name);  // FormData / URLSearchParams (any window)
    return null;
  };
  const codeFor = (e) => ({ 3: 404, 4: 400, 5: 403 }[e.status] || 500);

  // Returns {status, body, type} for the stock web UI's endpoints.
  async function route(method, url, body, onProgress, signal) {
    const u = new URL(url, 'http://reader/');
    const q = u.searchParams;
    try {
      if (method === 'GET' && u.pathname === '/api/files') {
        return { status: 200, type: 'application/json', body: JSON.stringify(await listDir(norm(q.get('path') || '/'))) };
      }
      if (method === 'GET' && u.pathname === '/api/status') {
        const s = JSON.parse(dec.decode(await link.call(CMD.STATUS)));
        return { status: 200, type: 'application/json', body: JSON.stringify(Object.assign(s, {
          firmwareFamily: 'crosspoint', pluginApi: 1, pluginCapabilities: '', ip: 'USB serial', mode: 'STA', rssi: 0 })) };
      }
      if (u.pathname === '/api/settings' || u.pathname === '/api/fonts' || u.pathname === '/api/fonts/delete') {
        const post = method === 'POST';
        const r = await callApi(post ? 1 : 0, u.pathname + u.search, post && typeof body === 'string' ? body : '');
        return { status: r.status, type: u.pathname === '/api/settings' && post ? 'text/plain' : 'application/json', body: r.text };
      }
      if (method === 'POST' && u.pathname === '/api/fonts/upload') {
        const file = body.get('file'), family = body.get('family');
        try {
          const data = new Uint8Array(await file.arrayBuffer());
          await writeFile('/@font/' + family + '/' + file.name.replace(/ /g, '_'), data, true, onProgress, signal);
          return { status: 200, type: 'application/json', body: '{"ok":true}' };
        } catch (e) {
          if (e instanceof DeviceError) return { status: 400, type: 'application/json', body: '{"error":"Invalid font file"}' };
          throw e;
        }
      }
      if (method === 'GET' && (u.pathname === '/api/plugins' || u.pathname === '/api/opds' || u.pathname === '/api/wifi')) {
        return { status: 200, type: 'application/json', body: '[]' };
      }
      if (method === 'POST' && u.pathname === '/upload') {
        const file = body.get('file');
        const data = new Uint8Array(await file.arrayBuffer());
        const overwrite = ['1', 'true'].includes(q.get('overwrite'));
        await writeFile(joinPath(norm(q.get('path') || '/'), file.name), data, overwrite, onProgress, signal);
        return { status: 200, type: 'text/plain', body: 'File uploaded successfully: ' + file.name };
      }
      if (method === 'POST' && u.pathname === '/mkdir') {
        await link.call(CMD.MKDIR, enc.encode(joinPath(norm(fieldOf(body, 'path') || '/'), fieldOf(body, 'name'))),
                        { what: 'mkdir', timeout: 10000 });
        return { status: 200, type: 'text/plain', body: 'Folder created' };
      }
      if (method === 'POST' && u.pathname === '/rename') {
        const src = norm(fieldOf(body, 'path'));
        await link.call(CMD.RENAME, cat(enc.encode(src), new Uint8Array([0]), enc.encode(joinPath(parentOf(src), fieldOf(body, 'name')))),
                        { what: 'rename', timeout: 10000 });
        return { status: 200, type: 'text/plain', body: 'Renamed' };
      }
      if (method === 'POST' && u.pathname === '/move') {
        const src = norm(fieldOf(body, 'path'));
        const dst = joinPath(norm(fieldOf(body, 'dest')), src.slice(src.lastIndexOf('/') + 1));
        await link.call(CMD.RENAME, cat(enc.encode(src), new Uint8Array([0]), enc.encode(dst)), { what: 'move', timeout: 10000 });
        return { status: 200, type: 'text/plain', body: 'Moved' };
      }
      if (method === 'POST' && u.pathname === '/delete') {
        const raw = fieldOf(body, 'paths');
        const paths = raw ? JSON.parse(raw) : [fieldOf(body, 'path')];
        const failed = [];
        for (const p of paths) {
          try { await link.call(CMD.DELETE, enc.encode(norm(p)), { what: 'delete', timeout: 10000 }); }
          catch (e) { failed.push(p + ' (' + e.message + ')'); }
        }
        return failed.length ? { status: 500, type: 'text/plain', body: 'Failed to delete some items: ' + failed.join('; ') }
                             : { status: 200, type: 'text/plain', body: 'All items deleted successfully' };
      }
      return { status: 404, type: 'text/plain', body: 'Not available over the serial cable' };
    } catch (e) {
      return { status: e instanceof DeviceError ? codeFor(e) : 503, type: 'text/plain', body: e.message };
    }
  }

  // ---- page host: each CrossPoint page runs in its own iframe whose network layer is replaced ----
  class FakeXHR {
    constructor() { this.upload = {}; this.status = 0; this.responseText = ''; this.readyState = 0; }
    open(method, url) { this.method = method.toUpperCase(); this.url = url; this.readyState = 1; }
    setRequestHeader() {}
    addEventListener(t, fn) { this['on' + t] = fn; }
    abort() { this.aborted = true; if (this.onabort) this.onabort(); }
    async send(body) {
      const xhr = this;
      await connectedPromise;
      const onProgress = (loaded, total) => {
        if (this.upload.onprogress) this.upload.onprogress({ lengthComputable: true, loaded, total });
        showProgress('Transferring ' + Math.round(100 * loaded / total) + '%');
      };
      const signal = { get aborted() { return xhr.aborted; } };
      const r = await route(this.method, this.url, body, onProgress, signal);
      showProgress('');
      if (this.aborted) return;
      this.status = r.status; this.responseText = r.body; this.readyState = 4;
      if (this.onload) this.onload();
    }
  }

  // The reader has no WebSocket upload server: fail fast so the page uses HTTP uploads.
  class FakeWS {
    constructor() {
      this.readyState = 3; this.bufferedAmount = 0;
      setTimeout(() => { if (this.onerror) this.onerror(new Event('error')); if (this.onclose) this.onclose({ code: 1006 }); }, 0);
    }
    send() {} close() {}
  }

  const pages = window.__cpxPages || {};
  const embedded = window.__cpxScripts || {};
  let frame = null, currentUrl = '/files';

  function install(win, search) {
    win.__cpxSearch = search;
    win.__cpxReload = () => navigate(currentUrl);
    win.fetch = async (input, init = {}) => {
      const url = typeof input === 'string' ? input : input.url;
      await connectedPromise;
      const r = await route((init.method || 'GET').toUpperCase(), url, init.body);
      return new Response(r.body, { status: r.status, headers: { 'Content-Type': r.type } });
    };
    // <img> loads of /download bypass fetch: read the file over serial and hand the image a Blob URL.
    const imgProto = win.HTMLImageElement.prototype;
    const imgSrc = Object.getOwnPropertyDescriptor(imgProto, 'src');
    Object.defineProperty(imgProto, 'src', {
      get() { return imgSrc.get.call(this); },
      set(v) {
        if (typeof v === 'string' && v.startsWith('/download')) {
          const img = this;
          const token = (img.__cpxToken = (img.__cpxToken || 0) + 1);
          readFile(norm(new URL(v, 'http://reader/').searchParams.get('path'))).then((blob) => {
            if (img.__cpxToken !== token) return;  // the preview moved on
            const url = URL.createObjectURL(blob);
            const done = () => URL.revokeObjectURL(url);  // the decoded image no longer needs it
            img.addEventListener('load', done, { once: true });
            img.addEventListener('error', done, { once: true });
            imgSrc.set.call(img, url);
          }).catch(() => {});
          return;
        }
        imgSrc.set.call(this, v);
      },
    });
    win.XMLHttpRequest = FakeXHR;
    win.WebSocket = FakeWS;
    // Scripts the page loads on demand from the reader (/js/...): serve embedded copies.
    const proto = win.HTMLScriptElement.prototype;
    const srcDesc = Object.getOwnPropertyDescriptor(proto, 'src');
    Object.defineProperty(proto, 'src', {
      get() { return srcDesc.get.call(this); },
      set(v) {
        const key = typeof v === 'string' ? v.split('?')[0] : '';
        if (embedded[key]) v = URL.createObjectURL(new Blob([embedded[key]], { type: 'text/javascript' }));
        srcDesc.set.call(this, v);
      },
    });
  }

  async function download(path) {
    try {
      const blob = await readFile(path, (l, t) => showProgress('Downloading ' + Math.round(100 * l / t) + '%'));
      const url = URL.createObjectURL(blob);
      const d = document.createElement('a');
      d.href = url; d.download = path.slice(path.lastIndexOf('/') + 1);
      document.body.appendChild(d); d.click(); d.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    } catch (e) { alert('Download failed: ' + e.message); }
    showProgress('');
  }

  function hook(doc) {
    doc.addEventListener('click', (ev) => {
      const a = ev.target.closest && ev.target.closest('a[href]');
      if (!a) return;
      const href = a.getAttribute('href') || '';
      if (a.classList.contains('image-preview-link')) return;  // the page opens its own preview
      if (href.startsWith('/download')) {
        ev.preventDefault();
        download(norm(new URL(href, 'http://reader/').searchParams.get('path')));
      } else if (href.startsWith('/') && !href.startsWith('//')) {
        ev.preventDefault();
        navigate(href);
      }
    }, true);
  }

  function navigate(url) {
    const u = new URL(url, 'http://reader/');
    const html = pages[u.pathname];
    if (!html) { alert('This page is not available over the serial cable (it needs WiFi).'); return; }
    currentUrl = u.pathname + u.search;
    const f = document.createElement('iframe');
    f.style.cssText = 'position:fixed;inset:0;width:100%;height:100%;border:0;background:#fff';
    document.body.insertBefore(f, document.body.firstChild);
    install(f.contentWindow, u.search);
    const d = f.contentDocument;
    d.open(); d.write(html); d.close();
    hook(d);
    if (frame) frame.remove();
    frame = f;
  }
})();
