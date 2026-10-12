// The File Manager's Details dialog: a book's title, author (and how it sorts), language, series,
// series index and description, saved to a "book.opf" sidecar, and its cover, saved as a
// "book.jpg" (or .png/.bmp) sidecar. The book itself is never changed.
//
// Grown from the metadata-editor plugin, which it replaces: built in now that the web pages live on
// the SD card and its size costs no firmware flash. Loaded by the File Manager on first use.
//
// The firmware prefers a sidecar over the metadata embedded in the book (docs/sidecar-files.md), so
// writing one is enough to change what the reader shows, for every format it opens: an EPUB, an
// XTC, and TXT and Markdown books, which have no metadata of their own. An existing sidecar is
// edited in place as XML rather than regenerated, so everything this dialog does not manage
// (publisher, identifiers, dates, the Calibre UUID) survives. Reading order matches the firmware's:
// an existing sidecar wins; otherwise an EPUB's own OPF, which the device hands out without the
// rest of the book (GET /api/book/opf).
(() => {
  const DC_NS = 'http://purl.org/dc/elements/1.1/';
  const OPF_NS = 'http://www.idpf.org/2007/opf';

  // The formats the reader opens. Must stay in step with the firmware's FsHelpers extension checks.
  const BOOK_EXTS = ['.epub', '.xtc', '.xtch', '.txt', '.md'];
  // Must stay in step with SidecarFiles::kCoverExtensions: the device takes the first of these that
  // exists, so writing a .jpg while an older .png remains would be decided by this order.
  const COVER_EXTS = ['.jpg', '.jpeg', '.png', '.bmp'];
  // The firmware ignores a metadata sidecar larger than this (docs/sidecar-files.md).
  const SIDECAR_MAX_BYTES = 16 * 1024;
  // The relay scope the firmware keeps for this dialog's cover search (DETAILS_RELAY_SCOPE).
  const RELAY_SCOPE = ':details';

  // name -> how it is stored in an OPF. "dc" elements carry their value as text; "meta" fields are
  // Calibre's <meta name=... content=.../> form; "fileAs" is the opf:file-as attribute of the
  // author's dc:creator, which the Library orders authors by. It must come after the author.
  const FIELDS = [
    { key: 'title', label: 'Title', kind: 'dc', tag: 'title' },
    { key: 'author', label: 'Author', kind: 'dc', tag: 'creator' },
    { key: 'authorSort', label: 'Sort author as', kind: 'fileAs', hint: 'e.g. Le Guin, Ursula K. Empty sorts by the last word.' },
    { key: 'language', label: 'Language', kind: 'dc', tag: 'language', hint: 'e.g. en, de' },
    { key: 'series', label: 'Series', kind: 'meta', metaName: 'calibre:series' },
    { key: 'seriesIndex', label: 'Series index', kind: 'meta', metaName: 'calibre:series_index' },
    { key: 'description', label: 'Description', kind: 'dc', tag: 'description', multiline: true }
  ];

  // The book being edited: { dir, name, base, opfName, sourceDoc, changed, overlay, onClosed }.
  let book = null;

  function open(path, onClosed) {
    close();
    const slash = path.lastIndexOf('/');
    const dir = slash > 0 ? path.slice(0, slash) : '/';
    const name = path.slice(slash + 1);
    book = { dir, name, base: stripExt(name), opfName: stripExt(name) + '.opf', sourceDoc: null, changed: false, onClosed };
    book.overlay = buildOverlay();
    document.body.appendChild(book.overlay);
    load();
  }

  function close() {
    if (!book) return;
    const { overlay, changed, onClosed } = book;
    book = null;
    overlay.remove();
    if (changed && onClosed) onClosed();
  }

  // ---- The dialog -------------------------------------------------------------------------------

  function buildOverlay() {
    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay open';
    overlay.addEventListener('click', (e) => { if (e.target === overlay) close(); });
    overlay.innerHTML =
      '<div class="modal" style="max-width:640px;max-height:90vh;overflow:auto">' +
      '<button class="modal-close" id="md-close">&times;</button>' +
      '<h3>ℹ️ ' + esc(book.name) + '</h3>' +
      '<p id="md-note" class="file-info"></p>' +
      '<div id="md-form"><p>Loading metadata...</p></div>' +
      '<hr><div id="md-cover"></div>' +
      // The Font Manager's converter can make a font family of the fonts an EPUB embeds.
      (hasExt(book.name, '.epub')
        ? '<hr><p class="file-info">Fonts embedded in this book can become a reading font: ' +
          '<a href="/fonts?book=' + encodeURIComponent(join(book.dir, book.name)) + '">Convert its fonts</a></p>'
        : '') +
      '</div>';
    overlay.querySelector('#md-close').onclick = close;
    return overlay;
  }

  const q = (id) => book && book.overlay.querySelector(id);

  async function load() {
    let values = {};
    try {
      const names = await folderNames();
      const opf = findName(names, book.base, ['.opf']);
      if (opf) book.opfName = opf;
      const twins = names.filter((n) => isBook(n) && n !== book.name && stripExt(n).toLowerCase() === book.base.toLowerCase());
      const notes = [];
      if (twins.length) {
        notes.push('These details also apply to ' + twins.join(', ') + ': a sidecar belongs to every book of the same name.');
      }
      if (opf) {
        book.sourceDoc = xml(await getText(dl(join(book.dir, opf))));
        values = readFrom(book.sourceDoc);
      } else if (hasExt(book.name, '.epub')) {
        values = readFrom(xml(await opfFromEpub(join(book.dir, book.name))));
        notes.push('Shown from the book itself. Saving writes ' + book.opfName + ' beside it.');
      } else if (hasExt(book.name, '.xtc') || hasExt(book.name, '.xtch')) {
        notes.push('No details saved yet: until you save some, the reader shows the title and author from the XTC file itself.');
      } else {
        notes.push('No details saved yet: a text book carries none of its own, so the reader shows its file name until you save some.');
      }
      if (!book) return;  // closed while loading
      q('#md-note').textContent = notes.join(' ');
    } catch (e) {
      if (!book) return;
      q('#md-form').innerHTML = '<p class="file-info">Could not read the details: ' + esc(msg(e)) + '</p>';
      return;
    }
    renderForm(values);
    renderCover(values);
  }

  function renderForm(values) {
    let html = '';
    for (const f of FIELDS) {
      const v = esc(values[f.key] || '');
      html += '<label style="display:block;margin:8px 0"><strong>' + esc(f.label) + '</strong>' +
        (f.hint ? ' <span style="opacity:.7;font-size:.9em">' + esc(f.hint) + '</span>' : '') + '<br>' +
        (f.multiline
          ? '<textarea id="md-f-' + f.key + '" rows="4" style="width:100%;box-sizing:border-box">' + v + '</textarea>'
          : '<input type="text" id="md-f-' + f.key + '" value="' + v + '" style="width:100%;box-sizing:border-box">') +
        '</label>';
    }
    html += '<p class="file-info" style="opacity:.8">Clearing a field removes it from the sidecar, so the book\'s own value shows again.</p>' +
      '<button class="folder-btn" id="md-save">Save details</button> <span id="md-saved"></span>';
    q('#md-form').innerHTML = html;
    q('#md-save').onclick = save;
    keepSortNameWithAuthor(values);
  }

  // The sort name files the author it was written for. Renaming the author while leaving it would
  // shelve the new name under the old one ("Neil Gaiman" under "Pratchett, Terry"), so until the sort
  // field is edited by hand it follows the author: kept for the loaded name, cleared for any other.
  function keepSortNameWithAuthor(values) {
    const authorIn = q('#md-f-author');
    const sortIn = q('#md-f-authorSort');
    const loadedAuthor = (values.author || '').trim();
    const loadedSort = values.authorSort || '';
    let sortEdited = false;
    sortIn.oninput = () => { sortEdited = true; };
    authorIn.oninput = () => {
      if (!sortEdited) sortIn.value = authorIn.value.trim() === loadedAuthor ? loadedSort : '';
    };
  }

  async function save() {
    const values = {};
    for (const f of FIELDS) values[f.key] = q('#md-f-' + f.key).value.trim();
    const saved = q('#md-saved');
    q('#md-save').disabled = true;
    saved.textContent = 'Saving...';
    try {
      // Edit the existing sidecar so unmanaged fields survive; only build a new document when there
      // is nothing to preserve.
      const doc = book.sourceDoc || freshDoc();
      writeInto(doc, values);
      const text = '<?xml version="1.0" encoding="utf-8"?>\n' +
        new XMLSerializer().serializeToString(doc.documentElement) + '\n';
      const size = new Blob([text]).size;
      if (size > SIDECAR_MAX_BYTES) {
        throw new Error('the details come to ' + Math.ceil(size / 1024) + ' KB, and the reader ignores more than ' +
          (SIDECAR_MAX_BYTES / 1024) + ' KB. Shorten the description.');
      }
      await upload(book.opfName, new Blob([text], { type: 'application/oebps-package+xml' }));
      book.sourceDoc = doc;
      book.changed = true;
      saved.textContent = 'Saved ' + book.opfName;
    } catch (e) {
      if (book) saved.textContent = 'Not saved: ' + msg(e);
    } finally {
      if (book) q('#md-save').disabled = false;
    }
  }

  // ---- Cover --------------------------------------------------------------------------------------
  //
  // A cover beside the book overrides the one embedded in it, exactly like the metadata sidecar.
  //
  // Images are read cross-origin only where the host allows it. Open Library serves its search and
  // its covers with CORS, so it is fetched directly. Goodreads has no public API (retired in 2020) and
  // its search page is scraped; Google Books' search sends CORS but its images do not. For those the
  // device fetches through its relay, for ONE thing: the bytes of the cover chosen. Previews are plain
  // <img> tags, because showing a cross-origin image was never restricted, only reading its pixels.
  // The clipboard covers every other site: copy an image in the browser, paste it here.

  const SOURCES = {
    openlibrary: { label: 'Open Library', search: searchOpenLibrary, direct: true },
    goodreads: { label: 'Goodreads', search: searchGoodreads, direct: false },
    google: { label: 'Google Books', search: searchGoogleBooks, direct: false }
  };

  function renderCover(values) {
    q('#md-cover').innerHTML =
      '<h3>Cover</h3>' +
      '<p id="md-cover-state" class="file-info">Checking...</p>' +
      '<div id="md-cover-preview"></div>' +
      '<p><label>Replace from a file <input type="file" id="md-cover-file" accept="image/*"></label></p>' +
      '<p id="md-paste" tabindex="0" style="border:1px dashed rgba(128,128,128,.6);padding:10px">' +
      'Click here and press Ctrl+V to paste an image. Works with any site: copy the image in your browser, ' +
      'then paste it here.</p>' +
      '<p><label>Search <select id="md-source">' +
      Object.entries(SOURCES).map(([k, s]) => '<option value="' + k + '">' + s.label + '</option>').join('') +
      '</select></label> <button class="folder-btn" id="md-search">Find covers</button> ' +
      '<button class="delete-btn-cancel" id="md-remove">Remove cover</button> <span id="md-cover-msg"></span></p>' +
      '<div id="md-results"></div>';
    refreshCoverState();
    q('#md-cover-file').onchange = async (e) => {
      const file = e.target.files && e.target.files[0];
      if (file) await installCover(file, file.name);
    };
    q('#md-paste').onpaste = async (e) => {
      for (const item of (e.clipboardData && e.clipboardData.items) || []) {
        if (item.type && item.type.indexOf('image/') === 0) {
          e.preventDefault();
          await installCover(item.getAsFile(), 'pasted.' + item.type.split('/')[1]);
          return;
        }
      }
      coverMsg('There is no image in the clipboard.');
    };
    q('#md-remove').onclick = removeCover;
    q('#md-search').onclick = () => searchCovers(q('#md-source').value, values);
  }

  function coverMsg(t) {
    const m = q('#md-cover-msg');
    if (m) m.textContent = t;
  }

  async function currentCoverName() {
    return findName(await folderNames(), book.base, COVER_EXTS);
  }

  async function refreshCoverState() {
    try {
      const name = await currentCoverName();
      if (!book) return;
      q('#md-cover-state').textContent = name ? 'Cover: ' + name : 'No cover file: the cover inside the book is used.';
      // Cache-busted: the file name stays the same when the image changes.
      q('#md-cover-preview').innerHTML = name
        ? '<img alt="cover" style="max-height:160px" src="' + dl(join(book.dir, name)) + '&t=' + Date.now() + '">'
        : '';
    } catch (e) {
      if (book) q('#md-cover-state').textContent = 'Could not read the folder: ' + msg(e);
    }
  }

  async function installCover(blob, sourceName) {
    if (!blob) return;
    const ext = coverExtFor(blob.type, sourceName);
    if (!ext) {
      coverMsg('Unsupported image type: use JPEG, PNG or BMP.');
      return;
    }
    coverMsg('Saving...');
    try {
      // Drop any other variant first, so exactly one cover sidecar exists.
      const existing = await currentCoverName();
      if (existing && existing.toLowerCase() !== (book.base + ext).toLowerCase()) await del(join(book.dir, existing));
      await upload(book.base + ext, blob);
      book.changed = true;
      coverMsg('Saved ' + book.base + ext);
      await refreshCoverState();
    } catch (e) {
      coverMsg('Not saved: ' + msg(e));
    }
  }

  async function removeCover() {
    coverMsg('');
    try {
      const name = await currentCoverName();
      if (!name) {
        coverMsg('There is no cover file to remove.');
        return;
      }
      if (!confirm('Delete ' + name + '? The cover inside the book will be used again.')) return;
      await del(join(book.dir, name));
      book.changed = true;
      coverMsg('Removed ' + name);
      await refreshCoverState();
    } catch (e) {
      coverMsg('Not removed: ' + msg(e));
    }
  }

  function coverExtFor(mime, name) {
    const m = (mime || '').toLowerCase();
    if (m === 'image/jpeg' || m === 'image/jpg') return '.jpg';
    if (m === 'image/png') return '.png';
    if (m === 'image/bmp' || m === 'image/x-ms-bmp') return '.bmp';
    const n = (name || '').toLowerCase();
    if (n.endsWith('.jpg') || n.endsWith('.jpeg')) return '.jpg';
    if (n.endsWith('.png')) return '.png';
    if (n.endsWith('.bmp')) return '.bmp';
    return '';
  }

  async function searchCovers(sourceKey, values) {
    const source = SOURCES[sourceKey] || SOURCES.openlibrary;
    const title = (q('#md-f-title').value || values.title || '').trim();
    const author = (q('#md-f-author').value || values.author || '').trim();
    if (!title) {
      coverMsg('Enter a title first.');
      return;
    }
    coverMsg('Searching ' + source.label + '...');
    q('#md-results').textContent = '';
    try {
      const hits = await source.search(title, author);
      if (!book) return;
      if (!hits.length) {
        coverMsg('No covers found on ' + source.label + '.');
        return;
      }
      renderHits(hits, source);
    } catch (e) {
      coverMsg(source.label + ' search failed: ' + msg(e));
    }
  }

  // hit = { thumb, full, label }
  function renderHits(hits, source) {
    const results = q('#md-results');
    coverMsg(hits.length + ' result(s): click one to use it');
    results.innerHTML = hits.map((h, i) =>
      '<figure style="display:inline-block;margin:4px;text-align:center;width:110px">' +
      '<img data-i="' + i + '" class="md-hit" style="max-height:140px;cursor:pointer"' +
      ' src="' + esc(h.thumb) + '" alt="" loading="lazy">' +
      '<figcaption style="font-size:.8em">' + esc(h.label || '') + '</figcaption>' +
      '</figure>').join('');
    results.querySelectorAll('.md-hit').forEach((img) => {
      img.onclick = async () => {
        const hit = hits[Number(img.dataset.i)];
        coverMsg('Fetching the cover...');
        try {
          const resp = source.direct ? await fetch(hit.full) : await relay(hit.full);
          if (!resp.ok) throw new Error('cover ' + resp.status);
          await installCover(await resp.blob(), 'cover.jpg');
          if (book) q('#md-results').textContent = '';
        } catch (e) {
          coverMsg('Could not fetch that cover: ' + msg(e));
        }
      };
    });
  }

  async function searchOpenLibrary(title, author) {
    const url = 'https://openlibrary.org/search.json?limit=8&fields=title,author_name,cover_i' +
      '&title=' + encodeURIComponent(title) + (author ? '&author=' + encodeURIComponent(author) : '');
    const data = await (await fetch(url)).json();
    return (data.docs || []).filter((d) => d.cover_i).map((d) => ({
      thumb: 'https://covers.openlibrary.org/b/id/' + d.cover_i + '-M.jpg',
      full: 'https://covers.openlibrary.org/b/id/' + d.cover_i + '-L.jpg',
      label: (d.author_name || []).join(', ')
    }));
  }

  async function searchGoodreads(title, author) {
    const query = title + (author ? ' ' + author : '');
    const resp = await relay('https://www.goodreads.com/search?q=' + encodeURIComponent(query));
    if (!resp.ok) throw new Error('search ' + resp.status);
    const doc = new DOMParser().parseFromString(await resp.text(), 'text/html');
    return Array.from(doc.querySelectorAll('img.bookCover')).slice(0, 8).map((img) => {
      const thumb = img.getAttribute('src') || '';
      // Search results are ~50px wide. Goodreads encodes the size as a "._SX50_" segment before the
      // extension; dropping it yields the full-size cover.
      const full = thumb.replace(/\._S[XY]\d+_(?=\.[a-z]+$)/i, '');
      return { thumb, full, label: (img.getAttribute('alt') || '').trim() };
    }).filter((h) => h.thumb);
  }

  async function searchGoogleBooks(title, author) {
    const query = 'intitle:' + title + (author ? ' inauthor:' + author : '');
    const url = 'https://www.googleapis.com/books/v1/volumes?maxResults=8&q=' + encodeURIComponent(query);
    const data = await (await fetch(url)).json();
    return (data.items || []).map((item) => {
      const info = item.volumeInfo || {};
      const links = info.imageLinks || {};
      const thumb = links.thumbnail || links.smallThumbnail;
      if (!thumb) return null;
      // Served over http in the payload, and edge=curl draws a page-curl effect.
      const clean = thumb.replace(/^http:/, 'https:').replace(/&edge=curl/, '');
      return { thumb: clean, full: clean, label: (info.authors || []).join(', ') };
    }).filter(Boolean);
  }

  // ---- OPF read/write ---------------------------------------------------------------------------

  function readFrom(doc) {
    const out = {};
    for (const f of FIELDS) {
      if (f.kind === 'dc') {
        const n = dcElement(doc, f.tag);
        out[f.key] = n ? (n.textContent || '').trim() : '';
      } else if (f.kind === 'fileAs') {
        out[f.key] = readFileAs(doc);
      } else {
        const n = metaElement(doc, f.metaName);
        out[f.key] = n ? (n.getAttribute('content') || '').trim() : '';
      }
    }
    return out;
  }

  function writeInto(doc, values) {
    const metadata = metadataElement(doc);
    for (const f of FIELDS) {
      const value = values[f.key];
      if (f.kind === 'fileAs') {
        writeFileAs(doc, value);
        continue;
      }
      if (f.kind === 'dc') {
        let n = dcElement(doc, f.tag);
        if (!value) { if (n && n.parentNode) n.parentNode.removeChild(n); continue; }
        if (!n) {
          n = doc.createElementNS(DC_NS, 'dc:' + f.tag);
          metadata.appendChild(n);
        }
        n.textContent = value;
      } else {
        let n = metaElement(doc, f.metaName);
        if (!value) { if (n && n.parentNode) n.parentNode.removeChild(n); continue; }
        if (!n) {
          n = doc.createElementNS(OPF_NS, 'meta');
          n.setAttribute('name', f.metaName);
          metadata.appendChild(n);
        }
        n.setAttribute('content', value);
      }
    }
    // The firmware also accepts the EPUB 3 spelling of series. Leaving a stale one behind would give
    // two sources of truth whose winner depends on document order, so drop them once we manage the
    // Calibre pair.
    for (const prop of ['belongs-to-collection', 'group-position']) {
      Array.from(doc.getElementsByTagName('meta'))
        .filter((m) => m.getAttribute('property') === prop)
        .forEach((m) => m.parentNode && m.parentNode.removeChild(m));
    }
  }

  function metadataElement(doc) {
    const found = doc.getElementsByTagName('metadata')[0] || doc.getElementsByTagNameNS(OPF_NS, 'metadata')[0];
    if (found) return found;
    const m = doc.createElementNS(OPF_NS, 'metadata');
    doc.documentElement.appendChild(m);
    return m;
  }

  function dcElement(doc, tag) {
    return doc.getElementsByTagNameNS(DC_NS, tag)[0] || doc.getElementsByTagName('dc:' + tag)[0] || null;
  }

  function metaElement(doc, name) {
    return Array.from(doc.getElementsByTagName('meta')).find((m) => m.getAttribute('name') === name) || null;
  }

  // EPUB 3 states a creator's filing name in <meta refines="#id" property="file-as">.
  function fileAsRefinements(doc, creator) {
    const id = creator.getAttribute('id');
    if (!id) return [];
    return Array.from(doc.getElementsByTagName('meta'))
      .filter((m) => m.getAttribute('refines') === '#' + id && m.getAttribute('property') === 'file-as');
  }

  // The author's filing name: opf:file-as on its dc:creator (EPUB 2, Calibre), or the EPUB 3
  // refinement, which the firmware lets win when both are there.
  function readFileAs(doc) {
    const creator = dcElement(doc, 'creator');
    if (!creator) return '';
    const refined = fileAsRefinements(doc, creator)[0];
    if (refined) return (refined.getAttribute('content') || refined.textContent || '').trim();
    return (creator.getAttributeNS(OPF_NS, 'file-as') || creator.getAttribute('opf:file-as') || '').trim();
  }

  // Written as opf:file-as on the author's dc:creator, the form Calibre uses. Any EPUB 3 refinement
  // of it goes, so there is one source of truth; with no author there is nothing to file.
  function writeFileAs(doc, value) {
    const creator = dcElement(doc, 'creator');
    if (!creator) return;
    fileAsRefinements(doc, creator).forEach((m) => m.parentNode && m.parentNode.removeChild(m));
    creator.removeAttributeNS(OPF_NS, 'file-as');
    creator.removeAttribute('opf:file-as');
    if (!value) return;
    // The firmware reads the attribute by its literal name, "opf:file-as". A document that never
    // declared the prefix (EPUB 3 style) would otherwise be serialized with a generated one
    // ("ns1:file-as") that it does not know.
    const metadata = metadataElement(doc);
    if (metadata.lookupNamespaceURI('opf') !== OPF_NS) {
      metadata.setAttributeNS('http://www.w3.org/2000/xmlns/', 'xmlns:opf', OPF_NS);
    }
    creator.setAttributeNS(OPF_NS, 'opf:file-as', value);
  }

  function freshDoc() {
    return xml(
      '<?xml version="1.0" encoding="utf-8"?>\n' +
      '<package xmlns="' + OPF_NS + '" version="2.0" unique-identifier="uuid_id">\n' +
      '  <metadata xmlns:dc="' + DC_NS + '" xmlns:opf="' + OPF_NS + '"/>\n' +
      '</package>\n');
  }

  // The EPUB's own OPF. The device reads it out of the book (GET /api/book/opf); only when that fails,
  // for want of memory for example, is the whole book downloaded and opened here.
  async function opfFromEpub(path) {
    try {
      const r = await fetch('/api/book/opf?path=' + encodeURIComponent(path));
      if (r.ok) {
        const text = await r.text();
        xml(text);  // a truncated response fails here and falls through to the download
        return text;
      }
    } catch (e) { /* fall through */ }
    if (typeof JSZip === 'undefined') throw new Error('the book could not be read');
    const r = await fetch(dl(path));
    if (!r.ok) throw new Error('download ' + r.status);
    const zip = await JSZip.loadAsync(await r.blob());
    const containerFile = zip.file('META-INF/container.xml');
    if (!containerFile) throw new Error('no container.xml');
    const rootfile = xml(await containerFile.async('string')).getElementsByTagName('rootfile')[0];
    const opfPath = rootfile && rootfile.getAttribute('full-path');
    const opfFile = opfPath && zip.file(opfPath);
    if (!opfFile) throw new Error('no OPF in the book');
    return opfFile.async('string');
  }

  // ---- Helpers ----------------------------------------------------------------------------------

  async function folderNames() {
    const entries = await getJson('/api/files?path=' + encodeURIComponent(book.dir));
    return entries.filter((e) => !e.isDirectory).map((e) => e.name);
  }

  // The actual name of `base` + one of `exts` in `names`, in the order of `exts`, case-blind as FAT is
  // (the firmware accepts "book.OPF" and "book.JPG" too); null when there is none.
  function findName(names, base, exts) {
    const lower = new Map(names.map((n) => [n.toLowerCase(), n]));
    for (const ext of exts) {
      const hit = lower.get((base + ext).toLowerCase());
      if (hit) return hit;
    }
    return null;
  }

  async function upload(filename, blob) {
    const fd = new FormData();
    // /upload replaces a file of the same name, which is what saving means here.
    fd.append('file', blob, filename);
    const r = await fetch('/upload?path=' + encodeURIComponent(book.dir), { method: 'POST', body: fd });
    if (!r.ok) throw new Error('upload ' + r.status + ' ' + (await r.text().catch(() => '')));
  }

  async function del(path) {
    const fd = new FormData();
    fd.append('path', path);
    fd.append('type', 'file');
    const r = await fetch('/delete', { method: 'POST', body: fd });
    if (!r.ok) throw new Error('delete ' + r.status + ' ' + (await r.text().catch(() => '')));
  }

  function relay(url) {
    return fetch('/api/relay?plugin=' + encodeURIComponent(RELAY_SCOPE) + '&url=' + encodeURIComponent(url));
  }

  function xml(text) {
    const doc = new DOMParser().parseFromString(text, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length) throw new Error('malformed XML');
    return doc;
  }

  async function getJson(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + ' -> ' + r.status);
    return r.json();
  }

  async function getText(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + ' -> ' + r.status);
    return r.text();
  }

  function dl(p) { return '/download?path=' + encodeURIComponent(p); }
  function join(d, name) { return (d === '/' ? '' : d) + '/' + name; }
  function stripExt(n) { const d = n.lastIndexOf('.'); return d > 0 ? n.slice(0, d) : n; }
  function hasExt(n, ext) { return n.toLowerCase().endsWith(ext); }
  function isBook(n) { return BOOK_EXTS.some((ext) => hasExt(n, ext)); }
  function msg(e) { return (e && e.message) || String(e); }
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  window.MetadataEditor = { open, isBook };
})();
