  // --- browser-side plugin host (shared by Settings + File Manager) ---------
  // Discovers plugins from the SD card (/api/plugins), loads each plugin.js,
  // and calls its registered render fn with a container + a small api. A plugin
  // is just JS on the SD; adding one needs no firmware change.
  window.CrossPoint = window.CrossPoint || {
    _pending: null,
    registerPlugin(fn) { this._pending = fn; },
    // --- plugin job queue -------------------------------------------------
    // A plugin registers named actions; external systems enqueue jobs with
    // POST /api/plugin-jobs, and any open page hosting the plugin (File
    // Manager, Settings, or the headless /plugins-run page) claims and
    // executes them here, then posts the result back for the caller to poll.
    _actions: {},
    _actionBusy: false,
    _actionTimer: null,
    _pendingCompletion: null,
    registerAction(pluginName, action, fn) {
      (this._actions[pluginName] = this._actions[pluginName] || {})[action] = fn;
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
            try { result = (await fn(job.args || {})) || {}; ok = true; }
            catch (e) { result = { error: String((e && e.message) || e) }; }
          }
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
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        const err = new Error(path + ' ' + r.status + (detail ? ': ' + detail : ''));
        err.status = r.status;
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
      return { status: Number(r.headers.get('X-Relay-Status')), headers: list, body: await r.text() };
    },
    crypto(op, fields) { return this._post('/api/crypto', Object.assign({ op }, fields || {})); },
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
    async fetchToSd(plugin, url, dest, headers) {
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
        if (!Number.isFinite(result.bytes) || result.bytes <= offset) {
          throw new Error('/api/fetch made no progress');
        }
        offset = result.bytes;
      }
    }
  };

  async function loadPlugins(mount) {
    const host = document.getElementById('plugin-container');
    if (!host) return;
    let list;
    try { list = await (await fetch('/api/plugins')).json(); } catch (e) { return; }
    for (const p of list) {
      // A null mount loads everything: the headless runner page hosts all
      // plugins so any registered action can execute there.
      if (mount && (p.mount || 'settings') !== mount) continue;
      const container = document.createElement('div');
      container.className = 'card';
      container.id = 'plugin-' + p.name;
      host.appendChild(container);
      CrossPoint._pending = null;
      try {
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
          s.onload = resolve; s.onerror = () => reject(new Error('load ' + p.name));
          document.body.appendChild(s);
        });
        if (typeof CrossPoint._pending === 'function') {
          CrossPoint._pending(container, {
            name: p.name,
            relay: (m, u, h, b) => CrossPoint.relay(p.name, m, u, h, b),
            crypto: (op, f) => CrossPoint.crypto(op, f),
            writeFile: (path, d) => CrossPoint.writeFile(p.name, path, d),
            fetchToSd: (u, dest, h) => CrossPoint.fetchToSd(p.name, u, dest, h),
            registerAction: (a, fn) => CrossPoint.registerAction(p.name, a, fn),
            pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) + '&file=' + encodeURIComponent(f)
          });
        }
      } catch (e) {
        const h = document.createElement('h2');
        h.textContent = p.title || p.name;
        const msg = document.createElement('p');
        msg.style.color = '#c0392b';
        msg.textContent = 'Failed to load plugin.';
        container.replaceChildren(h, msg);
      }
    }
  }
