// --- browser-side plugin host (Plugins, File Manager, /plugins-run) -------
// Discovers plugins from the SD card (/api/plugins), loads each plugin.js,
// and calls its registered render fn with a container + a small api. A plugin
// is just JS on the SD; adding one needs no firmware change.
window.CrossPoint = window.CrossPoint || {
  // The render fn is stored on the registering script element, so a timed-out
  // script that runs late cannot hand its fn to the plugin loading after it.
  registerPlugin(fn) { if (document.currentScript) document.currentScript.cpRender = fn; },
  // --- plugin job queue -------------------------------------------------
  // A plugin registers named actions; external systems enqueue jobs with
  // POST /api/plugin-jobs, and any open page hosting the plugin (File
  // Manager, Plugins, or the headless /plugins-run page) claims and
  // executes them here, then posts the result back for the caller to poll.
  _actions: Object.create(null),  // no inherited keys: a plugin may be named "constructor"
  _actionBusy: false,
  _actionTimer: null,
  _pendingCompletion: null,
  registerAction(pluginName, action, fn) {
    (this._actions[pluginName] = this._actions[pluginName] || Object.create(null))[action] = fn;
    if (!this._actionTimer) this._actionTimer = setInterval(() => this._pollJobs(), 2500);
  },
  async _pollJobs() {
    if (this._actionBusy) return;
    this._actionBusy = true;
    try {
      if (this._pendingCompletion && !(await this._sendCompletion())) return;
      for (const plugin of Object.keys(this._actions)) {
        let job;
        try { job = await (await fetch('/api/plugin-jobs/claim?plugin=' + encodeURIComponent(plugin))).json(); }
        catch (e) { continue; }
        if (!job || !job.id) continue;
        const fn = this._actions[plugin][job.action];
        let ok = false;
        let result;
        if (!fn) {
          result = { error: 'unknown action: ' + job.action };
        } else {
          try { result = (await fn(job.args || {})) ?? {}; ok = true; }
          catch (e) { result = { error: String((e && e.message) || e) }; }
        }
        // An unsendable result would block the queue: completion retries forever.
        try { JSON.stringify(result); }
        catch (e) { ok = false; result = { error: 'result is not JSON-serializable: ' + e.message }; }
        this._pendingCompletion = { id: job.id, claim: job.claim, ok, result };
        if (!(await this._sendCompletion())) return;
      }
    } finally {
      this._actionBusy = false;
    }
  },
  // Posts the pending completion; false means retry on the next poll. A 4xx
  // is final (stale claim after a lease expiry, recycled job): drop it
  // rather than retry forever. Transport errors and 5xx retry.
  async _sendCompletion() {
    try {
      await this._post('/api/plugin-jobs/complete', this._pendingCompletion);
    } catch (e) {
      if (!(e.status >= 400 && e.status < 500)) return false;
    }
    this._pendingCompletion = null;
    return true;
  },
  async _readResponse(r, path) {
    if (!r.ok) {
      let result = null;
      try { result = await r.json(); } catch (e) {}
      const detail = (result && result.error) || '';
      const err = new Error(path + ' ' + r.status + (detail ? ': ' + detail : ''));
      err.status = r.status;
      if (result) err.result = result;  // the device's JSON answer, e.g. {error: 'body over 4KB'}
      throw err;
    }
    return r.json();
  },
  async _post(path, body) {
    return this._readResponse(await fetch(path, {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    }), path);
  },
  // Device capabilities a page can't do itself. relay() resolves to
  // { status, body, headers }, where headers is an ordered list of
  // [name, value] pairs (duplicates preserved, so every Set-Cookie shows up).
  async relay(plugin, method, url, headers, body) {
    const r = await fetch('/api/relay', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ plugin, method, url, headers: headers || {}, body: body || '' })
    });
    if (!r.ok) return this._readResponse(r, '/api/relay');  // throws with the device's error
    // The device sends the upstream body raw. Header values arrive as Latin-1
    // code units, so re-decode the UTF-8 header JSON.
    const raw = r.headers.get('X-Relay-Headers') || '[]';
    const list = JSON.parse(new TextDecoder().decode(Uint8Array.from(raw, (c) => c.charCodeAt(0))));
    let text;
    try {
      text = await r.text();
    } catch (e) {
      // The device streams the body and closes early when it is cut short or
      // over the cap: report that like the 502 it sends before streaming.
      const err = new Error('/api/relay 502: relay failed; large bodies need /api/fetch');
      err.status = 502;
      throw err;
    }
    return { status: Number(r.headers.get('X-Relay-Status')), headers: list, body: text };
  },
  crypto(op, fields) { return this._post('/api/crypto', Object.assign({}, fields, { op })); },
  async writeFile(plugin, path, dataB64) {
    // Decode in the browser and send the bytes as a multipart file part: the
    // device streams file parts to SD, so embedded NULs and large bodies
    // survive (a plain body would become a NUL-terminated string).
    const bytes = Uint8Array.from(atob(dataB64 || ''), (c) => c.charCodeAt(0));
    const url = '/api/plugin-fs?plugin=' + encodeURIComponent(plugin) +
      '&path=' + encodeURIComponent(path);
    const form = new FormData();
    form.append('file', new Blob([bytes]), 'data');
    const r = await fetch(url, { method: 'POST', body: form });
    return this._readResponse(r, '/api/plugin-fs');
  },
  async fetchToSd(plugin, url, dest, headers, options) {
    const method = ((options && options.method) || 'GET').toUpperCase();
    const body = (options && options.body) || '';
    // The device sends a non-GET request, or one with a body, once, without segments.
    // A failure throws, as on the GET path, with the device's result attached.
    if (method !== 'GET' || body) {
      const result = await this._post('/api/fetch', { plugin, url, dest, headers: headers || {}, method, body });
      if (result.complete === false) {
        const err = new Error('/api/fetch: ' + (result.error || 'failed') + ' (HTTP ' + result.status + ')');
        err.result = result;
        throw err;
      }
      return result;
    }
    // Keep each browser-facing request short. Mobile browsers may terminate
    // a request after a few minutes even when response heartbeats are flowing,
    // while the device itself is still downloading successfully.
    const maxBytes = 2 * 1024 * 1024;
    let offset = 0;
    for (;;) {
      const result = await this._post('/api/fetch', {
        plugin, url, dest, headers: headers || {}, offset, maxBytes
      });
      if (result.complete !== false) return result;
      if (result.error) {
        // The device dropped the partial file; resuming would only hide this error.
        const err = new Error('/api/fetch: ' + result.error + (result.status ? ' (HTTP ' + result.status + ')' : ''));
        err.result = result;
        throw err;
      }
      if (!Number.isFinite(result.bytes) || result.bytes <= offset) {
        throw new Error('/api/fetch made no progress');
      }
      offset = result.bytes;
    }
  }
};

// A script that neither loads nor fails in this time is shown as failed, so
// one stalled request does not hold back the plugins after it.
const PLUGIN_SCRIPT_TIMEOUT_MS = 30000;

// beforeLoad runs once, before the first matching plugin script; a failure
// there does not stop the plugins from loading. Resolves to false when the
// container or the plugin list is missing.
async function loadPlugins(mount, beforeLoad) {
  const host = document.getElementById('plugin-container');
  if (!host) return false;
  let list;
  try { list = await (await fetch('/api/plugins')).json(); } catch (e) { return false; }
  if (!Array.isArray(list)) return false;  // e.g. {"error":"out of memory"}
  for (const p of list) {
    // A null mount loads everything: the headless runner page hosts all
    // plugins so any registered action can execute there.
    if (mount && (p.mount || 'settings') !== mount) continue;
    if (beforeLoad) {
      try { await beforeLoad(); } catch (e) { console.warn('Plugin preload failed:', e); }
      beforeLoad = null;
    }
    const container = document.createElement('div');
    container.className = 'card';
    container.id = 'plugin-' + p.name;
    host.appendChild(container);
    const showFailure = () => {
      const h = document.createElement('h2');
      h.textContent = p.title || p.name;
      const msg = document.createElement('p');
      msg.style.color = '#c0392b';
      msg.textContent = 'Failed to load plugin.';
      container.replaceChildren(h, msg);
    };
    const s = document.createElement('script');
    const render = () => {
      if (typeof s.cpRender !== 'function') return;
      const rendered = s.cpRender(container, {
        name: p.name,
        dir: p.dir,
        relay: (m, u, h, b) => CrossPoint.relay(p.name, m, u, h, b),
        crypto: (op, f) => CrossPoint.crypto(op, f),
        writeFile: (path, d) => CrossPoint.writeFile(p.name, path, d),
        fetchToSd: (u, dest, h, o) => CrossPoint.fetchToSd(p.name, u, dest, h, o),
        registerAction: (a, fn) => CrossPoint.registerAction(p.name, a, fn),
        pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) + '&file=' + encodeURIComponent(f)
      });
      // Not awaited: the next plugin loads while an async render runs.
      if (rendered && typeof rendered.then === 'function') {
        rendered.then(null, (e) => { console.warn('Plugin render failed:', p.name, e); showFailure(); });
      }
    };
    const loaded = new Promise((resolve, reject) => {
      s.onload = resolve;
      s.onerror = () => reject(new Error('load ' + p.name));
    });
    s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
    document.body.appendChild(s);
    let timer;
    const timedOut = new Promise((resolve) => { timer = setTimeout(resolve, PLUGIN_SCRIPT_TIMEOUT_MS, true); });
    try {
      if (await Promise.race([loaded.then(() => false), timedOut])) {
        // Move on to the next plugin, but still render this one if its script arrives.
        showFailure();
        loaded.then(() => {
          if (typeof s.cpRender !== 'function') return;
          container.replaceChildren();
          render();
        }).catch(showFailure);
        continue;
      }
      render();
    } catch (e) {
      console.warn('Plugin load failed:', p.name, e);
      showFailure();
    } finally {
      clearTimeout(timer);
    }
  }
  return true;
}
