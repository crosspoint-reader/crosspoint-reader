// Operations run synchronously between cooperative firmware steps. Open handles
// and reader/font state also protect resources held by suspended firmware tasks.
export const SD_LIMIT = 64 * 1024 * 1024;
const ROOT = '/fs_';
const MAX_ENTRIES = 8192;
const encoder = new TextEncoder();
const inside = (path, parent) => path === parent || path.startsWith(`${parent}/`);
const fail = (code, path = '') => { throw Object.assign(new Error(code), { code, path }); };

export function createSdCard(module) {
  const fs = module.FS;
  function pathFor(path) {
    if (typeof path !== 'string' || !path.startsWith('/') || /[\\\x00-\x1f\x7f]/.test(path) || encoder.encode(path).length > 512) fail('invalidPath', path);
    const parts = path === '/' ? [] : path.slice(1).split('/');
    if (parts.length > 32 || parts.some(p => !p || p === '.' || p === '..' || encoder.encode(p).length > 255)) fail('invalidPath', path);
    let full = ROOT;
    for (const part of parts) {
      full += `/${part}`;
      try { if (fs.isLink(fs.lstat(full).mode)) fail('invalidPath', path); }
      catch (error) { if (error.errno !== 44) throw error; } // ENOENT
    }
    return full;
  }
  function stat(path) {
    try { return fs.lstat(pathFor(path)); }
    catch (error) { if (error.errno === 44) return null; throw error; }
  }
  function walk(path = '/') {
    const entries = [];
    function visit(path, depth) {
      if (depth > 32 || entries.length >= MAX_ENTRIES) fail('tooManyFiles', path);
      const info = fs.lstat(pathFor(path));
      const directory = fs.isDir(info.mode);
      entries.push({ path, directory, size: directory ? 0 : info.size });
      if (directory) for (const name of fs.readdir(pathFor(path))) {
        if (name !== '.' && name !== '..') visit(`${path === '/' ? '' : path}/${name}`, depth + 1);
      }
    }
    visit(path, 0);
    return entries;
  }
  function inUse(path) {
    const book = module.UTF8ToString(module._preview_active_book());
    if (book && inside(book, path)) fail('activeBook', path);
    const font = module.UTF8ToString(module._preview_active_font());
    if (font && ['/fonts', '/.fonts'].some(root =>
      inside(`${root}/${font}`, path) || inside(path, `${root}/${font}`) ||
      path.replace(/\.(ttf|otf|ttc)$/i, '') === `${root}/${font}`)) fail('activeFont', path);
    const full = pathFor(path);
    if (fs.streams.some(stream => stream && inside(fs.getPath(stream.node), full))) fail('fileInUse', path);
  }
  function writable(path) {
    pathFor(path);
    if (path === '/' || inside(path, '/.crosspoint')) fail('protectedFile', path);
    inUse(path);
  }
  function cachesFor(entries) {
    return entries.filter(entry => !entry.directory && /\.(epub|txt|xtc|xtch)$/i.test(entry.path)).map(entry => {
      const hash = module.ccall('preview_path_hash', 'number', ['string'], [entry.path]) >>> 0;
      return `/.crosspoint/${/\.xtch?$/i.test(entry.path) ? 'xtc' : 'epub'}_${hash}`;
    }).filter(path => stat(path));
  }
  function remove(path) {
    for (const entry of walk(path).reverse()) {
      if (entry.directory) fs.rmdir(pathFor(entry.path));
      else fs.unlink(pathFor(entry.path));
    }
  }
  function parents(path) {
    const parts = path.split('/').slice(1, -1);
    let parent = '';
    for (const part of parts) {
      parent += `/${part}`;
      if (!stat(parent)) fs.mkdir(pathFor(parent));
      else if (!fs.isDir(stat(parent).mode)) fail('notDirectory', parent);
    }
  }
  return function execute({ operation, path = '/', destination, bytes, replace = false, entries }) {
    pathFor(path);
    if (operation === 'list') {
      if (!fs.isDir(fs.stat(pathFor(path)).mode)) fail('notDirectory', path);
      const all = walk();
      return {
        entries: all.filter(entry => entry.path !== '/' && (entry.path.slice(0, entry.path.lastIndexOf('/')) || '/') === path),
        used: all.reduce((sum, entry) => sum + entry.size, 0), limit: SD_LIMIT,
      };
    }
    if (operation === 'download') {
      if (!fs.isFile(fs.stat(pathFor(path)).mode)) fail('notFile', path);
      // readFile makes an owned copy: transferring it cannot detach MEMFS data.
      return { bytes: fs.readFile(pathFor(path)).buffer };
    }
    if (operation === 'snapshot') {
      const all = walk();
      if (all.reduce((sum, entry) => sum + entry.size, 0) > SD_LIMIT) fail('storageFull');
      return { entries: all.filter(entry => entry.path !== '/').map(entry => ({ ...entry,
        ...(entry.directory ? {} : { bytes: fs.readFile(pathFor(entry.path)).buffer }),
      })) };
    }
    if (operation === 'restore') {
      // Only used before setup() when restarting or changing device profiles.
      if (!Array.isArray(entries) || entries.length > MAX_ENTRIES || entries.reduce((n, e) => n + (e.bytes?.byteLength || 0), 0) > SD_LIMIT) fail('storageFull');
      for (const entry of entries) pathFor(entry.path);
      for (const entry of walk().filter(entry => entry.path !== '/').reverse()) {
        if (entry.directory) fs.rmdir(pathFor(entry.path)); else fs.unlink(pathFor(entry.path));
      }
      for (const entry of entries) {
        parents(entry.path);
        if (entry.directory) fs.mkdir(pathFor(entry.path));
        else fs.writeFile(pathFor(entry.path), new Uint8Array(entry.bytes), { canOwn: true });
      }
      return {};
    }
    writable(path);
    if (operation === 'mkdir') {
      if (stat(path)) fail('alreadyExists', path);
      if (walk().length >= MAX_ENTRIES) fail('tooManyFiles', path);
      fs.mkdir(pathFor(path));
    } else if (operation === 'write') {
      if (!(bytes instanceof ArrayBuffer)) fail('invalidFile', path);
      const previous = stat(path);
      if (previous && !fs.isFile(previous.mode)) fail('notFile', path);
      if (previous && !replace) fail('alreadyExists', path);
      const all = walk();
      if (all.length + path.split('/').length >= MAX_ENTRIES) fail('tooManyFiles', path);
      if (all.reduce((n, e) => n + e.size, 0) - (previous?.size || 0) + bytes.byteLength > SD_LIMIT) fail('storageFull', path);
      const caches = cachesFor([{ path, directory: false }]);
      caches.forEach(inUse);
      parents(path);
      fs.writeFile(pathFor(path), new Uint8Array(bytes), { canOwn: true });
      caches.forEach(remove);
    } else if (operation === 'rename' || operation === 'delete') {
      const files = walk(path);
      if (operation === 'rename') writable(destination);
      const caches = [...new Set(cachesFor([...files, ...(operation === 'rename' ? files.map(entry => ({
        ...entry, path: destination + entry.path.slice(path.length),
      })) : [])]))];
      caches.forEach(inUse);
      if (operation === 'rename') {
        if (inside(destination, path)) fail('invalidPath', destination);
        if (stat(destination)) fail('alreadyExists', destination);
        fs.rename(pathFor(path), pathFor(destination));
      } else remove(path);
      caches.forEach(remove);
    } else fail('invalidOperation');
    return {};
  };
}
