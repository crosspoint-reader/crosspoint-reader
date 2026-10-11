// --- browser-side plugin host (shared by Settings + File Manager) -----------
// Ported from crosspoint-reader PR #2734 ("feat: Add browser-side plugin system
// with SD card support", Justin Mitchell / @itsthisjustin). The discovery flow,
// the registerPlugin handshake and the mount convention are his design; this
// version leaves out crypto (the browser's crypto.subtle covers it) and the
// plugin job queue.
//
// Discovers plugins from the SD card via /api/plugins, loads each plugin.js,
// and hands it a container element to render into. A plugin is just JS on the
// card, so adding one needs no firmware build.
//
// Plugins run in the page with full same-origin authority: they call the web
// server's endpoints (/api/files, /mkdir, /move, /delete, /upload, /download)
// directly with fetch(). Beyond the card, the device fetches on a plugin's
// behalf through relay (GET /api/relay) and fetchToSd (POST /api/fetch), and
// writeFile (POST /api/plugin-fs) stores a small file. Both outbound calls reach
// only the hosts in the plugin's manifest "allowedHosts", redirects included.
window.CrossPoint = window.CrossPoint || {
  _pending: null,
  // Called by a plugin at top level; loadPlugins() picks the function up after
  // that script finishes loading. Safe because plugins load strictly one at a
  // time (see loadPlugins), so _pending never belongs to two plugins at once.
  registerPlugin(fn) { this._pending = fn; }
};

// Device capabilities this firmware implements. A plugin may declare what it
// needs as "requires" in its manifest, and one asking for something absent is
// refused with a readable card instead of failing somewhere inside its own code.
//
// "crypto" is deliberately absent: the browser's own crypto.subtle covers
// hashing, HMAC, AES and RSA, so a device-side implementation would duplicate it.
const PLUGIN_CAPABILITIES = ['relay', 'fetchToSd', 'writeFile', 'pluginFile'];

function missingCapabilities(plugin) {
  const required = Array.isArray(plugin.requires) ? plugin.requires : [];
  return required.filter((cap) => PLUGIN_CAPABILITIES.indexOf(cap) === -1);
}

// `mount` selects which page's plugins to load, matching the manifest's "mount"
// field ("settings" or "files"); plugins omitting it default to "settings".
//
// Strictly sequential: the device serves one connection at a time, and parallel
// requests on page load can stall a response long enough to trip the firmware
// watchdog. Await this alongside the page's other loaders, never beside them.
async function loadPlugins(mount) {
  const host = document.getElementById('plugin-container');
  if (!host) return;

  let list;
  try {
    list = await (await fetch('/api/plugins')).json();
  } catch (e) {
    return;  // no card, no plugins folder, or a device busy elsewhere
  }

  for (const p of list) {
    if ((p.mount || 'settings') !== mount) continue;
    if (p.disabled) continue;  // switched off on the Plugins page; still installed

    const container = document.createElement('div');
    container.className = 'card';
    container.id = 'plugin-' + p.name;
    host.appendChild(container);

    const missing = missingCapabilities(p);
    if (missing.length) {
      showPluginError(container, p, 'needs ' + missing.join(', ') +
        ', which this firmware does not provide.');
      continue;
    }

    CrossPoint._pending = null;
    try {
      await new Promise((resolve, reject) => {
        const s = document.createElement('script');
        s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
        s.onload = resolve;
        s.onerror = () => reject(new Error('load ' + p.name));
        document.body.appendChild(s);
      });
      if (typeof CrossPoint._pending !== 'function') {
        throw new Error(p.name + ' did not call registerPlugin()');
      }
      CrossPoint._pending(container, {
        name: p.name,
        // Fetch a URL the browser cannot reach itself, via the device. GET only,
        // and only to hosts this plugin's manifest lists in allowedHosts.
        // Resolves to a Response so callers can pick .blob()/.text()/.json().
        relay: async (url) => {
          const r = await fetch('/api/relay?plugin=' + encodeURIComponent(p.name) +
            '&url=' + encodeURIComponent(url));
          if (!r.ok) throw new Error('relay ' + r.status + ': ' + (await r.text().catch(() => '')));
          return r;
        },
        // Download straight to the card - one transfer instead of relaying
        // through the browser and uploading the bytes back.
        fetchToSd: async (url, dest) => {
          const r = await fetch('/api/fetch?plugin=' + encodeURIComponent(p.name) +
            '&url=' + encodeURIComponent(url) + '&dest=' + encodeURIComponent(dest),
            { method: 'POST' });
          if (!r.ok) throw new Error('fetch ' + r.status + ': ' + (await r.text().catch(() => '')));
          return r.json();
        },
        // Write one small file. `data` may be a string, Blob or ArrayBuffer.
        writeFile: async (path, data) => {
          const r = await fetch('/api/plugin-fs?plugin=' + encodeURIComponent(p.name) +
            '&path=' + encodeURIComponent(path),
            { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: data });
          if (!r.ok) throw new Error('plugin-fs ' + r.status + ': ' + (await r.text().catch(() => '')));
          return r.json();
        },
        // Present so a plugin written against the upstream API fails with a
        // sentence rather than "api.crypto is not a function".
        crypto: () => {
          throw new Error('api.crypto is not available on this firmware - use the ' +
            "browser's crypto.subtle instead");
        },
        // manifest.json's title, falling back to the folder name. The host
        // renders no chrome - the plugin owns its whole card - so this is here
        // for a plugin that wants its heading to track the manifest instead of
        // hardcoding it.
        title: p.title || p.name,
        // URL of another file in this plugin's folder, for a plugin that ships
        // its own assets. Flat: the server serves no subdirectories.
        pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) +
          '&file=' + encodeURIComponent(f),
        // Folder the File Manager is showing, so a plugin can act on what the
        // user is already looking at instead of asking them to type a path
        // again. A getter, not a captured value, so it stays right if the page
        // ever navigates in place rather than reloading. Reads the host page's
        // global; typeof keeps it safe on Settings, which has no file browser
        // and where this is always '/'.
        get currentPath() {
          return typeof currentPath === 'string' ? currentPath : '/';
        }
      });
    } catch (e) {
      showPluginError(container, p, 'failed to load: ' + ((e && e.message) || e));
    }
  }
}

function showPluginError(container, plugin, detail) {
  const title = document.createElement('h2');
  title.textContent = plugin.title || plugin.name;
  const msg = document.createElement('p');
  msg.className = 'plugin-error';
  msg.textContent = 'This plugin ' + detail;
  container.replaceChildren(title, msg);
}
