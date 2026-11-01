// EPUB optimizer for the File Manager: repairs EPUB metadata and re-encodes,
// splits, crops and rotates images. FilesPage.html loads it on demand, after
// JSZip; it reads that page's top-level conversion settings (JPEG_QUALITY,
// MAX_WIDTH, HANDEDNESS, imageStates, ...) and log helpers.

// ═══════════════════════════════════════════════════════════════════
// EPUB Utilities — ported from EPUB Optimizer Pro
// ═══════════════════════════════════════════════════════════════════

/** Defensive CSS injected into every XHTML <head> — prevents e-ink overflow. */
const DEFENSIVE_STYLE = '<style type="text/css">img,svg{max-width:100%;height:auto}body{overflow-wrap:break-word}table{max-width:100%;table-layout:fixed}pre,code{white-space:pre-wrap;word-wrap:break-word}*{box-sizing:border-box}</style>';

/** Escape a string for safe insertion into XML attribute values / text content. */
function xmlEscape(str) {
  return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function escapeRegex(str) { return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

/**
 * Decode a URI-encoded href (e.g., "my%20image.jpg" → "my image.jpg").
 * Handles double-encoding gracefully.
 */
function decodeHref(href) {
  try { return decodeURIComponent(href); }
  catch (e) { return href; }
}

/**
 * Safely read a text file from the zip, handling BOM and encoding.
 * Strips UTF-8 BOM. Detects encoding from XML declaration or meta tag.
 */
async function safeReadText(fileObj) {
  const raw = await fileObj.async('uint8array');

  // Detect and strip UTF-8 BOM (EF BB BF)
  let offset = 0;
  if (raw.length >= 3 && raw[0] === 0xEF && raw[1] === 0xBB && raw[2] === 0xBF) {
    offset = 3;
  }

  // Try UTF-8 first (vast majority of EPUBs)
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  try {
    return utf8.decode(raw.subarray(offset));
  } catch (e) { /* not valid UTF-8 */ }

  // Peek at XML declaration or meta charset for encoding hint
  const ascii = new TextDecoder('ascii', { fatal: false }).decode(raw.subarray(offset, offset + 512));
  const encodingMatch = ascii.match(/encoding=["']([^"']+)["']/i) ||
                        ascii.match(/charset=["']?([^"'\s;]+)/i);
  const encoding = encodingMatch ? encodingMatch[1].toLowerCase() : 'windows-1252';

  try {
    return new TextDecoder(encoding, { fatal: false }).decode(raw.subarray(offset));
  } catch (e) {
    // Last resort: lossy latin1
    return new TextDecoder('iso-8859-1', { fatal: false }).decode(raw.subarray(offset));
  }
}

/**
 * Find the canonical OPF path by parsing META-INF/container.xml.
 * Falls back to scanning for any .opf file.
 */
async function findOPFPath(zip) {
  try {
    const containerPath = Object.keys(zip.files).find(p => p.toLowerCase() === 'meta-inf/container.xml');
    if (containerPath) {
      const containerXml = await zip.files[containerPath].async('string');
      const match = containerXml.match(/<rootfile[^>]+full-path=["']([^"']+)["']/i);
      if (match && zip.files[match[1]]) return match[1];
    }
  } catch (e) { /* fall through */ }
  let fallback = null;
  zip.forEach(p => { if (!fallback && p.toLowerCase().endsWith('.opf')) fallback = p; });
  return fallback;
}

function sanitizeMetadataFilenamePart(value) {
  const text = String(value || '').replace(/\s+/g, ' ').trim();
  return (text.normalize ? text.normalize('NFC') : text)
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, ' ')
    .replace(/\s+/g, ' ')
    .replace(/^[. ]+/g, '')
    .replace(/[. ]+$/g, '')
    .trim();
}

function buildMetadataFilename(title, author) {
  title = sanitizeMetadataFilenamePart(title);
  author = sanitizeMetadataFilenamePart(author);
  if (!title) return '';
  let base = author ? `${title} - ${author}` : title;
  if (base.length > 180) base = base.substring(0, 180).replace(/\s+\S*$/g, '').trim() || base.substring(0, 180).trim();
  return `${base}.epub`;
}

async function getMetadataFilenameForEpub(file) {
  if (typeof JSZip === 'undefined') return '';
  const zip = await JSZip.loadAsync(file);
  const opfPath = await findOPFPath(zip);
  if (!opfPath || !zip.files[opfPath]) return '';

  const doc = new DOMParser().parseFromString(await safeReadText(zip.files[opfPath]), 'application/xml');
  if (doc.querySelector('parsererror')) return '';

  const title = doc.getElementsByTagNameNS('*', 'title')[0]?.textContent || '';
  const creators = Array.from(doc.getElementsByTagNameNS('*', 'creator'));
  const getCreatorRole = el => (
    el.getAttribute('role') ||
    el.getAttribute('opf:role') ||
    el.getAttributeNS('http://www.idpf.org/2007/opf', 'role') ||
    ''
  ).toLowerCase();
  const authorEl = creators.find(el => getCreatorRole(el) === 'aut') ||
                   creators.find(el => !getCreatorRole(el));
  const authorText = authorEl?.textContent?.trim();
  const author = authorText ||
                 authorEl?.getAttribute('file-as') ||
                 authorEl?.getAttribute('opf:file-as') ||
                 authorEl?.getAttributeNS('http://www.idpf.org/2007/opf', 'file-as') ||
                 '';
  return buildMetadataFilename(title, author);
}

/**
 * Resolve a relative href against a base file path.
 * Handles multiple ../, ./, absolute /, and bare relative paths.
 */
function resolvePath(basePath, href) {
  if (href.startsWith('/')) return href.substring(1);
  href = href.replace(/^\.\//, '');
  const baseDir = basePath.includes('/') ? basePath.substring(0, basePath.lastIndexOf('/')) : '';
  const baseParts = baseDir ? baseDir.split('/') : [];
  const hrefParts = href.split('/');
  while (hrefParts.length > 0 && hrefParts[0] === '..') {
    hrefParts.shift();
    if (baseParts.length > 0) baseParts.pop();
  }
  const resolved = [...baseParts, ...hrefParts].join('/');
  return resolved.replace(/\/+/g, '/');
}

/** Remove XML comments from XHTML source text. Returns {text, count, bytes}. */
function stripComments(xml) {
  const SKIPPED = [
    { open: '<![CDATA[', close: ']]>' },
    { open: '<?', close: '?>' },
  ];

  const encoder = new TextEncoder();
  const out = [];
  let i = 0;
  let count = 0;
  let bytes = 0;

  const doctypeEnd = (from) => {
    const gt = xml.indexOf('>', from);
    const subset = xml.indexOf('[', from);
    if (subset < 0 || (gt >= 0 && subset > gt)) return gt < 0 ? xml.length : gt + 1;
    const close = xml.indexOf(']>', subset);
    return close < 0 ? xml.length : close + 2;
  };

  while (i < xml.length) {
    const next = xml.indexOf('<', i);
    if (next < 0) { out.push(xml.slice(i)); break; }
    if (next > i) out.push(xml.slice(i, next));

    if (xml.startsWith('<!--', next)) {
      const close = xml.indexOf('-->', next + 4);
      const end = close < 0 ? xml.length : close + 3;
      count++;
      bytes += encoder.encode(xml.slice(next, end)).length;
      i = end;
      continue;
    }

    if (xml.startsWith('<!DOCTYPE', next)) {
      const end = doctypeEnd(next + 9);
      out.push(xml.slice(next, end));
      i = end;
      continue;
    }

    const skipped = SKIPPED.find(s => xml.startsWith(s.open, next));
    if (skipped) {
      const close = xml.indexOf(skipped.close, next + skipped.open.length);
      const end = close < 0 ? xml.length : close + skipped.close.length;
      out.push(xml.slice(next, end));
      i = end;
      continue;
    }

    out.push('<');
    i = next + 1;
  }

  return { text: out.join(''), count, bytes };
}

/**
 * Serialize an XML doc back to string, preserving the original <?xml?> declaration
 * and cleaning up XMLSerializer namespace prefix noise (xmlns:ns0 etc).
 */
function safeSerialize(doc, originalContent) {
  let result = new XMLSerializer().serializeToString(doc);

  // Restore <?xml?> declaration if original had one
  if (originalContent && /^\s*<\?xml\b/.test(originalContent) && !/^\s*<\?xml\b/.test(result)) {
    const declMatch = originalContent.match(/^\s*(<\?xml[^?]*\?>)/);
    if (declMatch) result = declMatch[1] + '\n' + result;
  }

  // Clean up XMLSerializer namespace prefix noise (xmlns:ns0="..." ns0:attr="...")
  result = result.replace(/ xmlns:ns\d+="[^"]*"/g, '');
  result = result.replace(/ ns\d+:/g, ' ');

  return result;
}

/**
 * Extract main identifier from OPF for NCX sync. DOMParser with regex fallback.
 */
function extractIdentifier(opfContent) {
  let mainIdentifier = null;
  try {
    const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const pkg = doc.getElementsByTagNameNS('*', 'package')[0];
      const uid = pkg ? pkg.getAttribute('unique-identifier') : null;
      if (uid) {
        const el = [...doc.getElementsByTagNameNS('*', 'identifier')].find(e => e.getAttribute('id') === uid);
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
      if (!mainIdentifier) {
        const el = doc.getElementsByTagNameNS('*', 'identifier')[0];
        if (el) mainIdentifier = (el.textContent || '').trim();
      }
    }
  } catch (e) { /* fall through to regex */ }
  if (!mainIdentifier) {
    const uniqueIdMatch = opfContent.match(/<(?:\w+:)?package[^>]*unique-identifier=["']([^"']+)["']/i);
    if (uniqueIdMatch) {
      const idRegex = new RegExp(`<dc:identifier[^>]*id=["']${uniqueIdMatch[1]}["'][^>]*>([^<]+)</dc:identifier>`, 'i');
      const idMatch = opfContent.match(idRegex);
      if (idMatch) mainIdentifier = idMatch[1].trim();
    }
    if (!mainIdentifier) {
      const firstIdMatch = opfContent.match(/<dc:identifier[^>]*>([^<]+)</i);
      if (firstIdMatch) mainIdentifier = firstIdMatch[1].trim();
    }
  }
  return mainIdentifier;
}

/**
 * Sync NCX dtb:uid with the given identifier. DOMParser with regex fallback.
 */
function syncNCXIdentifier(ncxText, mainIdentifier) {
  if (!mainIdentifier) return ncxText;
  let t = ncxText;
  try {
    const doc = new DOMParser().parseFromString(t, 'application/xml');
    if (!doc.querySelector('parsererror')) {
      const meta = [...doc.getElementsByTagNameNS('*', 'meta')].find(m => m.getAttribute('name') === 'dtb:uid');
      if (meta) {
        meta.setAttribute('content', mainIdentifier);
        t = safeSerialize(doc, ncxText);
      }
    }
  } catch (e) {
    t = t.replace(/<meta\s+name=["']dtb:uid["']\s+content=["'][^"']*["']\s*\/?>/gi, `<meta name="dtb:uid" content="${xmlEscape(mainIdentifier)}"/>`);
  }
  return t;
}

// ===== Embedded font removal =====
// CrossPoint renders with built-in / SD-card fonts and never loads fonts
// embedded in an EPUB, so they are dead weight (often 100KB-2MB per book).
const FONT_EXT_REGEX = /\.(ttf|otf|woff2?|eot)$/;
const FONT_MEDIA_TYPE_REGEX = /^(font\/|application\/font-|application\/x-font-|application\/vnd\.ms-(opentype|fontobject)$)/i;

// Returns a Set of zip-root-relative paths of embedded font files, detected by
// file extension and by OPF manifest media-type (catches odd extensions).
function collectFontPaths(zip, opfContent, opfPath) {
  const fontPaths = new Set();
  zip.forEach(p => { if (FONT_EXT_REGEX.test(p.toLowerCase())) fontPaths.add(p); });

  if (opfContent && opfPath) {
    try {
      const doc = new DOMParser().parseFromString(opfContent, 'application/xml');
      if (!doc.querySelector('parsererror')) {
        for (const item of [...doc.getElementsByTagNameNS('*', 'item')]) {
          const type = item.getAttribute('media-type') || '';
          const href = item.getAttribute('href') || '';
          if (href && FONT_MEDIA_TYPE_REGEX.test(type)) {
            const resolved = resolvePath(opfPath, decodeHref(href));
            if (zip.files[resolved]) fontPaths.add(resolved);
          }
        }
      }
    } catch (e) {
      // Extension-based detection already covers the common cases
    }
  }
  return fontPaths;
}

// Strips all @font-face rules from CSS. Rules can also embed fonts directly as
// multi-hundred-KB data: URIs, so this is a size win even without font files.
function stripFontFaceRules(css) {
  let count = 0;
  const stripped = css.replace(/@font-face(?:\s|\/\*[\s\S]*?\*\/)*\{[^{}]*\}/gi, () => { count++; return ''; });
  return { css: stripped, count };
}

// Removes font-obfuscation <EncryptedData> entries from META-INF/encryption.xml.
// Returns { dropFile: true } when nothing meaningful remains, { modified, xml }
// when some entries were removed, or { modified: false } to keep the file as-is.
function stripFontEncryptionEntries(xmlText, fontPaths) {
  try {
    const doc = new DOMParser().parseFromString(xmlText, 'application/xml');
    if (doc.querySelector('parsererror')) return { modified: false };
    const entries = [...doc.getElementsByTagNameNS('*', 'EncryptedData')];
    let removed = 0;
    for (const entry of entries) {
      const ref = entry.getElementsByTagNameNS('*', 'CipherReference')[0];
      const uri = ref ? decodeHref(ref.getAttribute('URI') || '') : '';
      if (fontPaths.has(uri.replace(/^\//, ''))) {
        entry.parentNode.removeChild(entry);
        removed++;
      }
    }
    if (!removed) return { modified: false };
    const remaining = doc.getElementsByTagNameNS('*', 'EncryptedData').length +
                      doc.getElementsByTagNameNS('*', 'EncryptedKey').length;
    if (remaining === 0) return { dropFile: true };
    return { modified: true, xml: safeSerialize(doc, xmlText) };
  } catch (e) {
    return { modified: false };
  }
}

/**
 * Fix OPF content: fix media-types, strip svg properties,
 * update split image manifest entries, remove stripped font
 * entries, ensure cover meta. DOMParser with regex fallback.
 */
function fixOPF(opfText, opfOriginal, opfDir, splitImages = {}, fontPaths = null) {
  let t = opfText;

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(t, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('OPF parse failed');

    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    const manifestEl = doc.getElementsByTagNameNS('*', 'manifest')[0];

    // Fix media-types for converted images
    for (const item of items) {
      const href = item.getAttribute('href') || '';
      const type = item.getAttribute('media-type') || '';
      if (href.endsWith('.jpg') && type.match(/^image\/(png|gif|webp|bmp)$/)) {
        item.setAttribute('media-type', 'image/jpeg');
      }
    }

    // Remove 'svg' from properties
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      if (props.includes('svg')) {
        const newProps = props.split(/\s+/).filter(p => p !== 'svg').join(' ').trim();
        if (newProps) item.setAttribute('properties', newProps);
        else item.removeAttribute('properties');
      }
    }

    // Remove manifest entries for stripped embedded fonts
    if (fontPaths && fontPaths.size) {
      for (const item of items) {
        const href = decodeHref(item.getAttribute('href') || '');
        const full = resolvePath((opfDir ? opfDir + '/' : '') + 'x', href);
        if (fontPaths.has(full)) item.parentNode.removeChild(item);
      }
    }

    // Update split image hrefs and add manifest entries for parts
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');

      for (const item of items) {
        const h = item.getAttribute('href') || '';
        if (h === origHref || h === origHrefJpg || decodeHref(h) === origHref || decodeHref(h) === origHrefJpg) {
          item.setAttribute('href', part1Href);
          break;
        }
      }

      if (manifestEl) {
        const ns = manifestEl.namespaceURI || 'http://www.idpf.org/2007/opf';
        for (let j = 1; j < parts.length; j++) {
          const p = parts[j];
          const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
          const newItem = doc.createElementNS(ns, 'item');
          newItem.setAttribute('id', `img-${p.id}`);
          newItem.setAttribute('href', href);
          newItem.setAttribute('media-type', 'image/jpeg');
          manifestEl.appendChild(newItem);
        }
      }
    }

    t = safeSerialize(doc, opfOriginal);
  } catch (e) {
    // Regex fallback
    t = t.replace(/(<(?:\w+:)?item\b[^>]*href="[^"]+\.jpg"[^>]*)media-type="image\/(png|gif|webp|bmp)"/g, '$1media-type="image/jpeg"');
    t = t.replace(/(<(?:\w+:)?item\b[^>]*)media-type="image\/(png|gif|webp|bmp)"([^>]*href="[^"]+\.jpg")/g, '$1media-type="image/jpeg"$3');
    t = t.replace(/\s+svg(?=["'\s>])/g, '');
    if (fontPaths && fontPaths.size) {
      for (const p of fontPaths) {
        const href = opfDir && p.startsWith(opfDir + '/') ? p.substring(opfDir.length + 1) : p;
        const itemRegex = new RegExp(`<(?:\\w+:)?item\\b[^>]*href=["']${escapeRegex(href)}["'][^>]*\\/?>\\s*`, 'gi');
        t = t.replace(itemRegex, '');
      }
    }
    for (const [splitKey, splitInfo] of Object.entries(splitImages)) {
      const parts = splitInfo.parts || splitInfo;
      let origHref = opfDir && splitKey.startsWith(opfDir + '/') ? splitKey.substring(opfDir.length + 1) : splitKey;
      const origHrefJpg = origHref.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
      const part1Href = origHrefJpg.replace(/\.jpg$/i, '_part1.jpg');
      const origImgRegex = new RegExp(`(href=["'])(${escapeRegex(origHref)}|${escapeRegex(origHrefJpg)})(["'])`, 'gi');
      t = t.replace(origImgRegex, `$1${part1Href}$3`);
      let adds = '';
      for (let j = 1; j < parts.length; j++) {
        const p = parts[j];
        const href = opfDir && p.path.startsWith(opfDir + '/') ? p.path.substring(opfDir.length + 1) : p.path;
        adds += `<item id="img-${xmlEscape(p.id)}" href="${xmlEscape(href)}" media-type="image/jpeg"/>\n`;
      }
      if (adds && t.includes('</manifest>')) t = t.replace('</manifest>', adds + '</manifest>');
    }
  }

  // Ensure cover meta
  const cm = ensureCoverMeta(t);
  if (cm.fixed) t = cm.o;

  return t;
}

// Fix SVG cover - converts SVG-wrapped covers to plain HTML img tags
function fixSvgCover(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };
  if (!content.includes('calibre:cover') && !content.includes('name="cover"') && !content.includes('<title>Cover</title>')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
    }

    // Find SVG elements - check both standard and namespaced variants
    let imgHref = null;
    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    // Try to find all SVG elements
    const svgs = [
      ...doc.getElementsByTagName('svg'),
      ...doc.getElementsByTagNameNS(svgNS, 'svg'),
      ...doc.getElementsByTagName('svg:svg')
    ];

    for (const svg of svgs) {
      // Find image element inside - try all variants
      const imageEl = svg.getElementsByTagName('image')[0] ||
                      svg.getElementsByTagNameNS(svgNS, 'image')[0] ||
                      svg.getElementsByTagName('svg:image')[0];

      if (imageEl) {
        imgHref = imageEl.getAttributeNS(xlinkNS, 'href') ||
                  imageEl.getAttribute('xlink:href') ||
                  imageEl.getAttribute('href');
        if (imgHref) break;
      }
    }

    if (!imgHref) {
      // Fallback to regex
      const m = content.match(/xlink:href=["']([^"']+)["']/);
      if (!m) return { c: content, fixed: false, count: 0 };
      imgHref = m[1];
    }

    return {
      c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${imgHref}"/></section></body>
</html>`,
      fixed: true,
      count: 1
    };
  } catch (e) {
    // Fallback to regex
    const m = content.match(/xlink:href=["']([^"']+)["']/);
    if (!m) return { c: content, fixed: false, count: 0 };
    return { c: `<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en" xml:lang="en">
<head><meta content="text/html; charset=UTF-8" http-equiv="default-style"/><title>Cover</title></head>
<body><section epub:type="cover"><img style="max-width:100%;height:auto" alt="Cover" src="${m[1]}"/></section></body>
</html>`, fixed: true, count: 1 };
  }
}

// Fix SVG-wrapped images - unwrap SVG and replace with plain img
function fixSvgWrappedImages(content) {
  const hasSvg = content.includes('<svg') || content.includes('<svg:');
  if (!hasSvg || !content.includes('xlink:href')) return { c: content, fixed: false, count: 0 };

  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(content, 'application/xhtml+xml');

    if (doc.querySelector('parsererror')) {
      // Fallback to regex
      let fixedCount = 0;
      const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
      const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
      return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
    }

    const svgNS = 'http://www.w3.org/2000/svg';
    const xlinkNS = 'http://www.w3.org/1999/xlink';

    const svgElements = [...doc.querySelectorAll('svg'), ...doc.getElementsByTagNameNS(svgNS, 'svg')];
    const uniqueSvgs = [...new Set(svgElements)];
    let fixedCount = 0;

    for (const svg of uniqueSvgs) {
      const imageEl = svg.querySelector('image[*|href]') || svg.getElementsByTagNameNS(svgNS, 'image')[0] || svg.getElementsByTagNameNS('*', 'image')[0];
      if (!imageEl) continue;
      const href = imageEl.getAttributeNS(xlinkNS, 'href') || imageEl.getAttribute('xlink:href') || imageEl.getAttribute('href');
      if (!href) continue;
      const width = imageEl.getAttribute('width') || svg.getAttribute('width');
      const height = imageEl.getAttribute('height') || svg.getAttribute('height');
      const img = doc.createElementNS('http://www.w3.org/1999/xhtml', 'img');
      img.setAttribute('src', href);
      img.setAttribute('alt', '');
      img.setAttribute('style', 'max-width:100%;height:auto');
      if (width) img.setAttribute('width', width);
      if (height) img.setAttribute('height', height);
      svg.parentNode.replaceChild(img, svg);
      fixedCount++;
    }

    if (fixedCount === 0) return { c: content, fixed: false, count: 0 };
    return { c: safeSerialize(doc, content), fixed: true, count: fixedCount };

  } catch (e) {
    // Fallback to regex
    let fixedCount = 0;
    const svgImageRegex = /<(?:svg:)?svg\b[^>]*>[\s\S]*?<(?:svg:)?image\b[^>]*xlink:href=["']([^"']+)["'][^>]*\/?>\s*<\/(?:svg:)?svg>/gi;
    const newContent = content.replace(svgImageRegex, (match, href) => { fixedCount++; return `<img style="max-width:100%;height:auto" src="${href}" alt="" />`; });
    return { c: newContent, fixed: fixedCount > 0, count: fixedCount };
  }
}

// Ensure cover meta tag exists in OPF — DOMParser with regex fallback
function ensureCoverMeta(opfString) {
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(opfString, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('Parse failed');

    // Find cover image id: properties="cover-image", or id/href containing "cover"
    let coverId = null;
    const items = [...doc.getElementsByTagNameNS('*', 'item')];
    for (const item of items) {
      const props = item.getAttribute('properties') || '';
      const id = item.getAttribute('id') || '';
      const type = item.getAttribute('media-type') || '';
      if (!type.startsWith('image/')) continue;
      if (props.includes('cover-image')) { coverId = id; break; }
    }
    if (!coverId) {
      for (const item of items) {
        const id = item.getAttribute('id') || '';
        const href = item.getAttribute('href') || '';
        const type = item.getAttribute('media-type') || '';
        if (!type.startsWith('image/')) continue;
        if (id.toLowerCase().includes('cover') || href.toLowerCase().includes('cover')) { coverId = id; break; }
      }
    }
    if (!coverId) return { o: opfString, fixed: false };

    // Find or create <meta name="cover" content="..."/>
    const metas = [...doc.getElementsByTagNameNS('*', 'meta')];
    const coverMeta = metas.find(m => m.getAttribute('name') === 'cover');
    if (coverMeta) {
      if (coverMeta.getAttribute('content') === coverId) return { o: opfString, fixed: false };
      coverMeta.setAttribute('content', coverId);
    } else {
      const metadata = doc.getElementsByTagNameNS('*', 'metadata')[0];
      if (!metadata) return { o: opfString, fixed: false };
      const ns = metadata.namespaceURI || 'http://www.idpf.org/2007/opf';
      const newMeta = doc.createElementNS(ns, 'meta');
      newMeta.setAttribute('name', 'cover');
      newMeta.setAttribute('content', coverId);
      metadata.appendChild(newMeta);
    }
    return { o: safeSerialize(doc, opfString), fixed: true };
  } catch (e) {
    // Regex fallback
    return ensureCoverMetaRegex(opfString);
  }
}

function ensureCoverMetaRegex(o) {
  let coverId = null, m;
  if (!coverId && (m = o.match(/<\w+:?item[^>]+id="([^"]+)"[^>]+properties="[^"]*cover-image[^"]*"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]+properties="[^"]*cover-image[^"]*"[^>]+id="([^"]+)"/i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]+)"[^>]*href="[^"]*cover[^"]*"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*href="[^"]*cover[^"]*"[^>]*id="([^"]+)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*id="([^"]*cover[^"]*)"[^>]*media-type="image\//i))) coverId = m[1];
  if (!coverId && (m = o.match(/<\w+:?item[^>]*media-type="image\/[^"]*"[^>]*id="([^"]*cover[^"]*)"/i))) coverId = m[1];
  if (!coverId) return { o, fixed: false };
  const metaMatch = o.match(/<\w+:?meta\s+name=["']cover["']\s+content=["']([^"']+)["']/i) || o.match(/<\w+:?meta\s+content=["']([^"']+)["']\s+name=["']cover["']/i);
  if (metaMatch) {
    if (metaMatch[1] === coverId && !metaMatch[1].includes('/')) return { o, fixed: false };
    const esc = xmlEscape(coverId);
    o = o.replace(/<\w+:?meta\s+name=["']cover["']\s+content=["'][^"']+["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    o = o.replace(/<\w+:?meta\s+content=["'][^"']+["']\s+name=["']cover["']\s*\/?>/gi, `<meta name="cover" content="${esc}" />`);
    return { o, fixed: true };
  }
  const idx = o.indexOf('</metadata>');
  if (idx !== -1) return { o: o.substring(0, idx) + `    <meta name="cover" content="${xmlEscape(coverId)}"/>\n  </metadata>` + o.substring(idx + 11), fixed: true };
  return { o, fixed: false };
}

// Auto-crop tuning.
const CROP_WHITE_THRESHOLD = 245;
const CROP_BACKGROUND_TOLERANCE = 28;
const CROP_BACKGROUND_MAX_SPREAD = 24;
const CROP_EDGE_SAMPLE_SIZE = 12;
const CROP_PADDING_PX = 8;
const MIN_CROP_SAVINGS_RATIO = 0.08;
const MIN_COLOR_CROP_SAVINGS_RATIO = 0.20;
const MIN_CROP_DIMENSION = 240;

// Apply grayscale to canvas image data
function applyGrayscale(ctx, width, height) {
  if (!ENABLE_GRAYSCALE) return;
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  for (let i = 0; i < data.length; i += 4) {
    // Alpha-blend against white background before grayscaling (handles transparent PNGs)
    const a = data[i + 3] / 255;
    const blendedR = data[i] * a + 255 * (1 - a);
    const blendedG = data[i + 1] * a + 255 * (1 - a);
    const blendedB = data[i + 2] * a + 255 * (1 - a);
    const gray = Math.round(blendedR * 0.299 + blendedG * 0.587 + blendedB * 0.114);
    data[i] = gray; data[i + 1] = gray; data[i + 2] = gray; data[i + 3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
}

function shouldSkipAutoCrop(imagePath, width, height, ignoreToggle = false) {
  if (!ignoreToggle && !ENABLE_AUTO_CROP) return true;
  if (autoCropProtectedPaths.has(imagePath)) return true;
  if (width < MIN_CROP_DIMENSION || height < MIN_CROP_DIMENSION) return true;
  return /(^|\/)(cover|thumbnail|thumb|icon)[^/]*\.(jpe?g|png|gif|webp|bmp)$/i.test(imagePath || '');
}

function blendedPixel(data, i) {
  const alpha = data[i + 3] / 255;
  return {
    r: data[i] * alpha + 255 * (1 - alpha),
    g: data[i + 1] * alpha + 255 * (1 - alpha),
    b: data[i + 2] * alpha + 255 * (1 - alpha)
  };
}

function estimateCropBackground(data, width, height) {
  const sampleSize = Math.min(CROP_EDGE_SAMPLE_SIZE, Math.floor(width / 8), Math.floor(height / 8));
  if (sampleSize < 2) return null;

  // Use corners plus edge midpoints so colored margins work without trimming full-bleed art.
  const samples = [];
  const points = [
    [0, 0],
    [width - sampleSize, 0],
    [0, height - sampleSize],
    [width - sampleSize, height - sampleSize],
    [Math.floor((width - sampleSize) / 2), 0],
    [Math.floor((width - sampleSize) / 2), height - sampleSize],
    [0, Math.floor((height - sampleSize) / 2)],
    [width - sampleSize, Math.floor((height - sampleSize) / 2)]
  ];

  for (const [startX, startY] of points) {
    let r = 0, g = 0, b = 0, count = 0;
    for (let y = startY; y < startY + sampleSize; y++) {
      const row = y * width * 4;
      for (let x = startX; x < startX + sampleSize; x++) {
        const px = blendedPixel(data, row + x * 4);
        r += px.r; g += px.g; b += px.b; count++;
      }
    }
    samples.push({ r: r / count, g: g / count, b: b / count });
  }

  const avg = samples.reduce((sum, px) => ({
    r: sum.r + px.r,
    g: sum.g + px.g,
    b: sum.b + px.b
  }), { r: 0, g: 0, b: 0 });
  avg.r /= samples.length;
  avg.g /= samples.length;
  avg.b /= samples.length;

  const maxSpread = samples.reduce((spread, px) => Math.max(
    spread,
    Math.abs(px.r - avg.r),
    Math.abs(px.g - avg.g),
    Math.abs(px.b - avg.b)
  ), 0);

  if (maxSpread > CROP_BACKGROUND_MAX_SPREAD) return null;
  return avg;
}

function isCropContentPixel(px, background) {
  if (background) {
    return Math.abs(px.r - background.r) > CROP_BACKGROUND_TOLERANCE ||
           Math.abs(px.g - background.g) > CROP_BACKGROUND_TOLERANCE ||
           Math.abs(px.b - background.b) > CROP_BACKGROUND_TOLERANCE;
  }
  return px.r < CROP_WHITE_THRESHOLD || px.g < CROP_WHITE_THRESHOLD || px.b < CROP_WHITE_THRESHOLD;
}

function isNearWhiteBackground(background) {
  return background &&
         background.r >= CROP_WHITE_THRESHOLD &&
         background.g >= CROP_WHITE_THRESHOLD &&
         background.b >= CROP_WHITE_THRESHOLD;
}

function findNonWhiteBounds(ctx, width, height) {
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;
  const background = estimateCropBackground(data, width, height);
  let left = width, top = height, right = -1, bottom = -1;

  for (let y = 0; y < height; y++) {
    const row = y * width * 4;
    for (let x = 0; x < width; x++) {
      const i = row + x * 4;
      const px = blendedPixel(data, i);

      if (isCropContentPixel(px, background)) {
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
      }
    }
  }

  if (right < left || bottom < top) return null;

  left = Math.max(0, left - CROP_PADDING_PX);
  top = Math.max(0, top - CROP_PADDING_PX);
  right = Math.min(width - 1, right + CROP_PADDING_PX);
  bottom = Math.min(height - 1, bottom + CROP_PADDING_PX);

  const cropW = right - left + 1;
  const cropH = bottom - top + 1;
  const savedRatio = 1 - ((cropW * cropH) / (width * height));
  // Colored-background crops need a bigger win to avoid shaving illustrated pages.
  const minSavedRatio = background && !isNearWhiteBackground(background)
    ? MIN_COLOR_CROP_SAVINGS_RATIO
    : MIN_CROP_SAVINGS_RATIO;
  if (savedRatio < minSavedRatio) return null;

  return { x: left, y: top, width: cropW, height: cropH };
}

function createAutoCroppedCanvas(img, imagePath, width, height, ignoreToggle = false) {
  // Build a white-backed source canvas, detect the uniform page/background color
  // from its edges, crop to pixels that differ from that background, then let the
  // normal resize/rotate/split pipeline fit that cropped content to the device.
  // If the image is a cover/icon/small asset, the edges are not uniform, or the
  // crop would save too little, return the original canvas unchanged.
  const sourceCanvas = document.createElement('canvas');
  sourceCanvas.width = width;
  sourceCanvas.height = height;
  const sourceCtx = sourceCanvas.getContext('2d');
  sourceCtx.fillStyle = '#FFF';
  sourceCtx.fillRect(0, 0, width, height);
  sourceCtx.drawImage(img, 0, 0);

  if (shouldSkipAutoCrop(imagePath, width, height, ignoreToggle)) {
    return { canvas: sourceCanvas, width, height };
  }

  const crop = findNonWhiteBounds(sourceCtx, width, height);
  if (!crop) {
    return { canvas: sourceCanvas, width, height };
  }

  const croppedCanvas = document.createElement('canvas');
  croppedCanvas.width = crop.width;
  croppedCanvas.height = crop.height;
  const croppedCtx = croppedCanvas.getContext('2d');
  croppedCtx.fillStyle = '#FFF';
  croppedCtx.fillRect(0, 0, crop.width, crop.height);
  croppedCtx.drawImage(sourceCanvas, crop.x, crop.y, crop.width, crop.height, 0, 0, crop.width, crop.height);

  return { canvas: croppedCanvas, width: crop.width, height: crop.height, crop };
}

// Process single image - returns array of {data, suffix} objects
const IMAGE_LOAD_TIMEOUT_MS = 30000; // 30 second timeout for image loading
async function processImage(data, imageState = 0, imagePath = '') {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(new Blob([data]));
    const img = new Image();
    const origSize = data.byteLength;
    // Set up timeout to handle cases where image never loads
    const timeoutId = setTimeout(() => {
      URL.revokeObjectURL(url);
      reject(new Error('Image load timeout'));
    }, IMAGE_LOAD_TIMEOUT_MS);

    img.onload = async () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      const origW = img.width, origH = img.height;
      const source = createAutoCroppedCanvas(img, imagePath, origW, origH);
      const sourceCanvas = source.canvas;
      const sourceW = source.width;
      const sourceH = source.height;
      const sourceWasCropped = !!source.crop;

      // imageState: 0=Normal, 1=H-Split (CW/CCW), 2=V-Split, 3=Rotate & Fit
      // ========================================================================
      // STATE 1: H-Split (Rotate + Split) - EXACT COPY FROM index.html
      // Step 1: Scale WIDTH to 800px (keep aspect ratio)
      // Step 2: Rotate 90° CW or CCW based on HANDEDNESS
      // Step 3: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      if (imageState === 1) {
        // Step 1: Scale WIDTH to 800 (this is the key difference!)
        const scale = MAX_HEIGHT / sourceW;  // 800 / sourceW
        const scaledW = MAX_HEIGHT;  // 800
        const scaledH = Math.round(sourceH * scale);

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(sourceCanvas, 0, 0, sourceW, sourceH, 0, 0, scaledW, scaledH);

        // Step 2: Rotate 90° CW or CCW
        const rotW = scaledH;
        const rotH = scaledW;  // 800

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          // Rotate 90° CW
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          // Rotate 90° CCW
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(scaledCanvas, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0); // Reset transform
        applyGrayscale(rotCtx, rotW, rotH);

        // Step 3: If WIDTH > 480, split vertically
        if (rotW <= MAX_WIDTH) {
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 1 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - from RIGHT to LEFT for CW, LEFT to RIGHT for CCW
          const parts = [];
          const maxW = MAX_WIDTH;  // 480
          
          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((rotW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((rotW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x;
            if (isClockwise) {
              // CW: right to left - start from right edge
              x = rotW - maxW - (i * step);
            } else {
              // CCW: left to right - start from left edge
              x = i * step;
            }
            // Clamp to valid range
            x = Math.max(0, Math.min(x, rotW - maxW));
            positions.push(x);
          }

          // Ensure first and last positions are at edges
          if (isClockwise) {
            positions[0] = rotW - maxW; // First part at right edge
            positions[numParts - 1] = 0; // Last part at left edge
          } else {
            positions[0] = 0; // First part at left edge
            positions[numParts - 1] = rotW - maxW; // Last part at right edge
          }

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = rotH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, rotH);
            // Draw the slice
            partCtx.drawImage(rotCanvas, x, 0, partW, rotH, 0, 0, partW, rotH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: rotH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: true, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 1 }
          });
        }
      }
      // ========================================================================
      // STATE 2: V-Split (Vertical Split, no rotation)
      // Step 1: Scale HEIGHT to 800px (up or down)
      // Step 2: If WIDTH > 480, split vertically with overlap
      // ========================================================================
      else if (imageState === 2) {
        // ALWAYS scale height to 800 (up or down)
        const scale = MAX_HEIGHT / sourceH;  // 800 / sourceH
        const scaledW = Math.round(sourceW * scale);
        const scaledH = MAX_HEIGHT;  // Always 800

        const scaledCanvas = document.createElement('canvas');
        scaledCanvas.width = scaledW;
        scaledCanvas.height = scaledH;
        const scaledCtx = scaledCanvas.getContext('2d');
        scaledCtx.imageSmoothingEnabled = true;
        scaledCtx.imageSmoothingQuality = 'high';
        scaledCtx.fillStyle = '#FFF';
        scaledCtx.fillRect(0, 0, scaledW, scaledH);
        scaledCtx.drawImage(sourceCanvas, 0, 0, sourceW, sourceH, 0, 0, scaledW, scaledH);
        applyGrayscale(scaledCtx, scaledW, scaledH);

        // Check if split needed
        if (scaledW <= MAX_WIDTH) {
          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: scaledW, height: scaledH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: scaledW, finalH: scaledH, finalSize: arrBuf.byteLength, imageState: 2 }
          });
        } else {
          // Split by WIDTH (vertical cuts) - LEFT to RIGHT (natural reading order)
          const parts = [];
          const maxW = MAX_WIDTH;
          
          // Centered distribution: calculate numParts first, then distribute evenly
          let overlapPx, step, numParts;
          const minOverlapPx = Math.round(maxW * (OVERLAP_PERCENT / 100));  // Configurable overlap
          const maxStep = maxW - minOverlapPx;
          numParts = Math.ceil((scaledW - minOverlapPx) / maxStep);
          if (numParts < 2) numParts = 2;
          // Now calculate step to distribute evenly
          step = Math.round((scaledW - maxW) / (numParts - 1));
          overlapPx = maxW - step;
          // Ensure minimum overlap
          if (overlapPx < minOverlapPx) {
            overlapPx = minOverlapPx;
            step = maxW - overlapPx;
          }

          // Calculate all x positions first to ensure consistency
          const positions = [];
          for (let i = 0; i < numParts; i++) {
            let x = i * step;
            // Clamp to valid range
            x = Math.max(0, Math.min(x, scaledW - maxW));
            positions.push(x);
          }
          // Ensure last position is at right edge
          positions[0] = 0;
          positions[numParts - 1] = scaledW - maxW;

          for (let i = 0; i < numParts; i++) {
            const x = positions[i];
            const partW = maxW; // Always full width for consistency

            const partCanvas = document.createElement('canvas');
            partCanvas.width = partW;
            partCanvas.height = scaledH;
            const partCtx = partCanvas.getContext('2d');
            // Clear canvas first
            partCtx.fillStyle = '#FFFFFF';
            partCtx.fillRect(0, 0, partW, scaledH);
            // Draw the slice
            partCtx.drawImage(scaledCanvas, x, 0, partW, scaledH, 0, 0, partW, scaledH);

            const blob = await new Promise(res => partCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
            const arrBuf = await blob.arrayBuffer();
            parts.push({ data: arrBuf, suffix: `_part${i + 1}`, width: partW, height: scaledH, size: arrBuf.byteLength });
          }

          const totalSize = parts.reduce((sum, p) => sum + p.size, 0);
          resolve({
            parts,
            meta: { origW, origH, origSize, wasSplit: true, splitCount: numParts, rotated: false, finalW: parts[0].width, finalH: parts[0].height, finalSize: totalSize, imageState: 2 }
          });
        }
      }
      // ========================================================================
      // STATE 3: Rotate & Fit (Rotate 90°, then scale to fit 480x800, no split)
      // ========================================================================
      else if (imageState === 3) {
        // Step 1: Rotate 90° based on handedness
        const rotW = sourceH;
        const rotH = sourceW;

        const rotCanvas = document.createElement('canvas');
        rotCanvas.width = rotW;
        rotCanvas.height = rotH;
        const rotCtx = rotCanvas.getContext('2d');
        rotCtx.fillStyle = '#FFF';
        rotCtx.fillRect(0, 0, rotW, rotH);

        const isClockwise = HANDEDNESS === 'right';
        if (isClockwise) {
          rotCtx.translate(rotW, 0);
          rotCtx.rotate(Math.PI / 2);
        } else {
          rotCtx.translate(0, rotH);
          rotCtx.rotate(-Math.PI / 2);
        }
        rotCtx.drawImage(sourceCanvas, 0, 0);
        rotCtx.setTransform(1, 0, 0, 1, 0, 0);

        // Step 2: Scale to fit 480x800 (if needed)
        const fitsInScreen = rotW <= MAX_WIDTH && rotH <= MAX_HEIGHT;
        
        if (fitsInScreen && !sourceWasCropped) {
          // Already fits after rotation - just apply grayscale
          applyGrayscale(rotCtx, rotW, rotH);
          const blob = await new Promise(res => rotCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: rotW, height: rotH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: rotW, finalH: rotH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / rotW, MAX_HEIGHT / rotH);
          const newW = Math.round(rotW * scale);
          const newH = Math.round(rotH * scale);

          const scaledCanvas = document.createElement('canvas');
          scaledCanvas.width = newW;
          scaledCanvas.height = newH;
          const scaledCtx = scaledCanvas.getContext('2d');
          scaledCtx.imageSmoothingEnabled = true;
          scaledCtx.imageSmoothingQuality = 'high';
          scaledCtx.fillStyle = '#FFF';
          scaledCtx.fillRect(0, 0, newW, newH);
          scaledCtx.drawImage(rotCanvas, 0, 0, newW, newH);
          applyGrayscale(scaledCtx, newW, newH);

          const blob = await new Promise(res => scaledCanvas.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: true, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 3 }
          });
        }
      }
      // ========================================================================
      // STATE 0: Normal processing (scale to fit, no split/rotation)
      // ========================================================================
      else {
        // Normal processing: check if scaling is needed
        const fitsInScreen = sourceW <= MAX_WIDTH && sourceH <= MAX_HEIGHT;

        if (fitsInScreen && !sourceWasCropped) {
          // Image already fits - just convert to JPEG with grayscale
          const c = document.createElement('canvas');
          c.width = sourceW;
          c.height = sourceH;
          const ctx = c.getContext('2d');
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, sourceW, sourceH);
          ctx.drawImage(sourceCanvas, 0, 0);
          applyGrayscale(ctx, sourceW, sourceH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: sourceW, height: sourceH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: sourceW, finalH: sourceH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        } else {
          // Scale to fit 480x800
          const scale = Math.min(MAX_WIDTH / sourceW, MAX_HEIGHT / sourceH);
          const newW = Math.round(sourceW * scale);
          const newH = Math.round(sourceH * scale);

          const c = document.createElement('canvas');
          c.width = newW;
          c.height = newH;
          const ctx = c.getContext('2d');
          ctx.imageSmoothingEnabled = true;
          ctx.imageSmoothingQuality = 'high';
          ctx.fillStyle = '#FFF';
          ctx.fillRect(0, 0, newW, newH);
          ctx.drawImage(sourceCanvas, 0, 0, newW, newH);
          applyGrayscale(ctx, newW, newH);

          const blob = await new Promise(res => c.toBlob(res, 'image/jpeg', JPEG_QUALITY / 100));
          const arrBuf = await blob.arrayBuffer();
          resolve({
            parts: [{ data: arrBuf, suffix: '', width: newW, height: newH, size: arrBuf.byteLength }],
            meta: { origW, origH, origSize, wasSplit: false, rotated: false, finalW: newW, finalH: newH, finalSize: arrBuf.byteLength, imageState: 0 }
          });
        }
      }
    };
    img.onerror = () => {
      clearTimeout(timeoutId);
      URL.revokeObjectURL(url);
      reject(new Error('Image load failed'));
    };
    img.src = url;
  });
}

// Convert EPUB file - returns converted blob
async function convertEpubFile(file, progressCallback) {
  const startTime = Date.now();
  const originalSize = file.size;

  // Initialize logging
  clearLog();
  showLog();
  log(`<strong>${file.name}</strong> <span class="log-detail">(${formatBytes(originalSize)})</span>`, '', 'INFO');
  log(`Quality: ${JPEG_QUALITY}% | Overlap: ${OVERLAP_PERCENT}% | Rotation: ${HANDEDNESS === 'right' ? 'CW' : 'CCW'} | Grayscale: ${ENABLE_GRAYSCALE ? 'ON' : 'OFF'} | Auto-crop: ${ENABLE_AUTO_CROP ? 'ON' : 'OFF'}`, '', 'INFO');

  const zip = await JSZip.loadAsync(file);
  const renamed = {};
  zip.forEach(p => {
    const l = p.toLowerCase();
    if (l.match(/\.(png|gif|webp|bmp|jpeg)$/)) {
      renamed[p] = p.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');
    }
  });

  const out = new JSZip();
  const entries = Object.entries(zip.files);
  const splitImages = {};
  const xhtmlFiles = {};
  let opfPath = null, opfContent = null;
  let mainIdentifier = null;

  // Write mimetype FIRST per EPUB OCF spec
  if (zip.files['mimetype']) {
    const mimetypeData = await zip.files['mimetype'].async('arraybuffer');
    out.file('mimetype', mimetypeData, { compression: 'STORE', createFolders: false });
  }

  // First pass: process images
  for (let i = 0; i < entries.length; i++) {
    if (operationCancelled) throw new Error('Cancelled by user');
    const [path, fileObj] = entries[i];
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();

    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/)) {
      const data = await fileObj.async('arraybuffer');
      const imageState = getImageState(path);

      let result;
      try {
        result = await processImage(data, imageState, path);
      } catch (imageError) {
        // Log error but continue with original image
        console.error(`Failed to process image ${path}:`, imageError);
        log(`Warning: Failed to process ${path.split('/').pop()}, using original`, 'warning', 'IMG-ERR');

        // Use original image data as fallback
        result = {
          parts: [{
            data: data,
            suffix: '',
            width: 0,
            height: 0,
            size: data.byteLength
          }],
          meta: {
            origW: 0,
            origH: 0,
            origSize: data.byteLength,
            wasSplit: false,
            rotated: false,
            finalW: 0,
            finalH: 0,
            finalSize: data.byteLength,
            imageState: imageState,
            processingError: true
          }
        };
      }

      const parts = result.parts;
      const meta = result.meta;

      const baseName = path.replace(/\.[^.]+$/, '');
      const newExt = '.jpg';

      // Log image processing
      const imgName = path.split('/').pop();
      const origFormat = path.split('.').pop();
      logImage(imgName, meta.origW, meta.origH, origFormat, meta.origSize, meta.finalW, meta.finalH, meta.finalSize, meta.wasSplit, meta.splitCount || 0, parts, meta.imageState || 0);

      if (parts.length === 1 && parts[0].suffix === '') {
        const newPath = renamed[path] || path.replace(/\.[^.]+$/, newExt);
        out.file(newPath, parts[0].data, { compression: 'STORE', createFolders: false });
      } else {
        // Store with full path for collision prevention, but also keep original filename
        const origName = path.split('/').pop();
        const origDir = path.includes('/') ? path.substring(0, path.lastIndexOf('/')) : '';

        // Key by full path to avoid collisions
        splitImages[path] = {
          origName: origName,
          origDir: origDir,
          parts: []
        };

        for (const part of parts) {
          const partName = baseName.split('/').pop() + part.suffix + newExt;
          const partPath = (path.includes('/') ? path.substring(0, path.lastIndexOf('/') + 1) : '') + partName;
          out.file(partPath, part.data, { compression: 'STORE', createFolders: false });
          // Store metadata for XHTML/OPF updates
          splitImages[path].parts.push({
            path: partPath,
            imgName: partName,
            id: baseName.split('/').pop() + part.suffix,
            suffix: part.suffix
          });
        }
      }
    } else if (low.match(/\.(xhtml|html|htm)$/)) {
      xhtmlFiles[path] = await safeReadText(fileObj);
    } else if (low.endsWith('.opf')) {
      opfPath = path;
      opfContent = await safeReadText(fileObj);
    }

    if (progressCallback) progressCallback((i / entries.length) * 60);
  }

  // Embedded fonts are dropped entirely — CrossPoint never renders with them.
  // Counters cover font files, @font-face rules (incl. data: URI fonts), and
  // encryption.xml shrinkage; they count uncompressed content bytes removed
  // (the final archive-size delta is reported separately by logSummary).
  const fontPaths = collectFontPaths(zip, opfContent, opfPath);
  let removedFontCount = 0;
  let removedFontBytes = 0;
  let removedFontFaceRules = 0;

  // Second pass: update XHTML using DOMParser
  for (const [xhtmlPath, content] of Object.entries(xhtmlFiles)) {
    if (operationCancelled) throw new Error('Cancelled by user');
    let t = content;

    const stripped = stripComments(t);
    if (stripped.count) {
      t = stripped.text;
      logFix(`Comments (${stripped.count}, ${formatBytes(stripped.bytes)})`, xhtmlPath.split('/').pop());
    }

    const r = fixSvgCover(t);
    if (r.fixed) { t = r.c; logFix('SVG cover', xhtmlPath.split('/').pop()); }

    const r2 = fixSvgWrappedImages(t);
    if (r2.fixed) { t = r2.c; logFix(`SVG images (${r2.count})`, xhtmlPath.split('/').pop()); }

    // Use DOMParser for all img modifications: remove width/height and handle split images
    try {
      const parser = new DOMParser();
      const doc = parser.parseFromString(t, 'application/xhtml+xml');
      const parseError = doc.querySelector('parsererror');

      if (!parseError) {
        let modified = false;

        // Remove width/height attributes from ALL img tags (dimensions may have changed)
        // This prevents CrossPoint and other readers from using wrong dimensions
        const allImgElements = doc.querySelectorAll('img');
        for (const img of allImgElements) {
          if (img.hasAttribute('width')) { img.removeAttribute('width'); modified = true; }
          if (img.hasAttribute('height')) { img.removeAttribute('height'); modified = true; }

          const src = img.getAttribute('src');
          if (src) {
            const decodedSrc = decodeHref(src);
            const resolvedSrc = resolvePath(xhtmlPath, decodedSrc);

            const match = Object.entries(renamed).find(([oldPath]) => resolvedSrc === oldPath);

            if (match) {
              const [oldPath, newPath] = match;
              img.setAttribute('src', decodedSrc.replace(oldPath.split('/').pop(), newPath.split('/').pop()));
              modified = true;
            }
          }
        }

        // Handle split images with path collision prevention
        if (Object.keys(splitImages).length > 0) {
          // Get XHTML directory for resolving relative paths
          const xhtmlDir = xhtmlPath.includes('/') ? xhtmlPath.substring(0, xhtmlPath.lastIndexOf('/')) : '';
          const rootFolders = ['ops', 'oebps', 'epub', 'content'];

          for (const [fullPath, splitInfo] of Object.entries(splitImages)) {
            const origName = splitInfo.origName;
            const origDir = splitInfo.origDir;
            const parts = splitInfo.parts;
            const newName = origName.replace(/\.(png|gif|webp|bmp|jpeg)$/i, '.jpg');

            // Extract immediate parent directory for collision prevention
            const splitDirParts = origDir.split('/').filter(p => p);
            const lastDir = splitDirParts.length > 0 ? splitDirParts[splitDirParts.length - 1].toLowerCase() : null;
            const immediateParent = (lastDir && !rootFolders.includes(lastDir)) ? splitDirParts[splitDirParts.length - 1] : null;

            // Get XHTML's parent directory parts for relative path resolution
            const xhtmlDirParts = xhtmlDir.split('/').filter(p => p);

            // Find all img elements
            const allImgs = doc.querySelectorAll('img');
            const matchingImgs = [];

            for (const img of allImgs) {
              const src = img.getAttribute('src') || '';
              const srcParts = src.split('/').filter(p => p && p !== '..' && p !== '.');
              const srcName = srcParts.pop() || '';

              // Check filename match
              if (srcName !== origName && srcName !== newName) continue;

              // Path collision prevention with root folder handling
              if (immediateParent) {
                // Image is in a specific subfolder (not root like OPS/OEBPS)
                if (srcParts.length === 0) {
                  // src has no path - check if XHTML is in same folder as image
                  const xhtmlLastDir = xhtmlDirParts.length > 0 ? xhtmlDirParts[xhtmlDirParts.length - 1] : null;
                  if (xhtmlLastDir !== immediateParent) continue;
                } else {
                  // src has path - verify parent directory matches
                  if (srcParts[srcParts.length - 1] !== immediateParent) continue;
                }
              } else {
                // Image is in root folder (like OEBPS/cover.jpg)
                // Only match if src has NO subfolder path OR points to root folder
                if (srcParts.length > 0) {
                  const srcLastDir = srcParts[srcParts.length - 1].toLowerCase();
                  if (!rootFolders.includes(srcLastDir)) continue;
                }
              }

              matchingImgs.push(img);
            }

            // Process each matching img — Pro's strip+inject approach
            for (const img of matchingImgs) {
              const src = img.getAttribute('src') || '';

              // Part 1: update src in-place, strip original sizing
              img.setAttribute('src', src.replace(origName, parts[0].imgName).replace(newName, parts[0].imgName));

              if (parts.length > 1) {
                // Strip original width/height/class that were sized for the unsplit image
                img.removeAttribute('width');
                img.removeAttribute('height');
                img.removeAttribute('class');
                img.setAttribute('style', 'max-width:100%;height:auto');

                // Neutralize container height constraints that were sized for the original
                let container = img.parentElement;
                const safeContainers = ['div', 'p', 'figure', 'aside', 'section', 'body'];
                while (container && !safeContainers.includes(container.tagName.toLowerCase())) container = container.parentElement;
                const insertTarget = container || img.parentElement;
                // Strip constraining classes/styles from container — they were for the unsplit image
                if (insertTarget && insertTarget.tagName.toLowerCase() !== 'body') {
                  insertTarget.removeAttribute('class');
                  insertTarget.removeAttribute('style');
                }
                const insertParent = insertTarget.parentElement;
                const insertRef = insertTarget.nextSibling;
                const ns = doc.documentElement.namespaceURI || 'http://www.w3.org/1999/xhtml';

                // Insert new minimal wrappers for parts 2+ in reading order
                for (let pi = 1; pi < parts.length; pi++) {
                  const wrapper = doc.createElementNS(ns, 'div');
                  const newImg = doc.createElementNS(ns, 'img');
                  const partSrc = src.replace(origName, parts[pi].imgName).replace(newName, parts[pi].imgName);
                  newImg.setAttribute('src', partSrc);
                  newImg.setAttribute('alt', '');
                  newImg.setAttribute('style', 'max-width:100%;height:auto');
                  wrapper.appendChild(newImg);
                  if (insertRef) insertParent.insertBefore(wrapper, insertRef);
                  else insertParent.appendChild(wrapper);
                }
              }
              modified = true;
            }
          }
        }

        // Only serialize if we made changes
        if (modified) {
          t = safeSerialize(doc, content);
        }
      }
    } catch (e) {
      console.warn('DOMParser error for', xhtmlPath, e.message);
    }

    // Strip @font-face rules from inline <style> blocks — embedded font files
    // are removed, so leftover rules would reference missing resources
    t = t.replace(/(<style\b[^>]*>)([\s\S]*?)(<\/style>)/gi, (m, open, css, close) => {
      const ff = stripFontFaceRules(css);
      if (!ff.count) return m;
      removedFontFaceRules += ff.count;
      removedFontBytes += new TextEncoder().encode(css).length - new TextEncoder().encode(ff.css).length;
      logFix('@font-face', `${ff.count} rule(s) removed from ${escapeHtml(xhtmlPath.split('/').pop())}`);
      return open + ff.css + close;
    });

    // Inject universal image constraint — prevents overflow on e-ink displays
    if (t.includes('</head>')) {
      t = t.replace('</head>', DEFENSIVE_STYLE + '</head>');
    }

    out.file(xhtmlPath, t, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // Extract main identifier from OPF using DOMParser with regex fallback
  if (opfContent) {
    mainIdentifier = extractIdentifier(opfContent);
  }

  // Third pass: update OPF using fixOPF (DOMParser with regex fallback)
  if (opfContent) {
    let t = opfContent;
    for (const [o, n] of Object.entries(renamed)) {
      t = t.split(o.split('/').pop()).join(n.split('/').pop());
    }
    const opfDir = opfPath.includes('/') ? opfPath.substring(0, opfPath.lastIndexOf('/')) : '';
    t = fixOPF(t, opfContent, opfDir, splitImages, fontPaths);
    if (t !== opfContent) logFix('OPF', 'manifest updated');
    out.file(opfPath, t, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  // Copy remaining files
  for (const [path, fileObj] of entries) {
    if (operationCancelled) throw new Error('Cancelled by user');
    if (fileObj.dir || path === 'mimetype') continue;
    const low = path.toLowerCase();
    if (low.match(/\.(png|gif|webp|bmp|jpg|jpeg)$/) || low.match(/\.(xhtml|html|htm)$/) || low.endsWith('.opf')) continue;

    if (fontPaths.has(path)) {
      const fontData = await fileObj.async('arraybuffer');
      removedFontCount++;
      removedFontBytes += fontData.byteLength;
      log(`${escapeHtml(path.split('/').pop())} <span class="log-detail">(${formatBytes(fontData.byteLength)})</span>`, 'success', 'REMOVE');
      continue;
    }

    let data = await fileObj.async('arraybuffer');
    if (low.endsWith('.css')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      const ff = stripFontFaceRules(t);
      if (ff.count) {
        removedFontFaceRules += ff.count;
        removedFontBytes += new TextEncoder().encode(t).length - new TextEncoder().encode(ff.css).length;
        t = ff.css;
        logFix('@font-face', `${ff.count} rule(s) removed from ${escapeHtml(path.split('/').pop())}`);
      }
      data = new TextEncoder().encode(t);
    } else if (path === 'META-INF/encryption.xml' && fontPaths.size) {
      const t = await safeReadText(fileObj);
      const enc = stripFontEncryptionEntries(t, fontPaths);
      if (enc.dropFile) {
        removedFontBytes += data.byteLength;
        logFix('encryption.xml', 'removed (contained only font obfuscation entries)');
        continue;
      }
      if (enc.modified) {
        const encoded = new TextEncoder().encode(enc.xml);
        removedFontBytes += Math.max(0, data.byteLength - encoded.length);
        data = encoded;
        logFix('encryption.xml', 'font obfuscation entries removed');
      }
    } else if (low.endsWith('.ncx')) {
      let t = await safeReadText(fileObj);
      for (const [o, n] of Object.entries(renamed)) {
        t = t.split(o.split('/').pop()).join(n.split('/').pop());
      }
      const oldT = t;
      t = syncNCXIdentifier(t, mainIdentifier);
      if (t !== oldT) logFix('NCX identifier', 'Synced with OPF');
      data = new TextEncoder().encode(t);
    }
    out.file(path, data, { compression: 'DEFLATE', compressionOptions: { level: 8 }, createFolders: false });
  }

  if (removedFontCount > 0 || removedFontFaceRules > 0) {
    log(`Removed ${removedFontCount} embedded font(s) and ${removedFontFaceRules} @font-face rule(s), ${formatBytes(removedFontBytes)} of font data`, '', 'INFO');
  }

  if (progressCallback) progressCallback(100);

  // Generate final blob
  const newBlob = await out.generateAsync({ type: 'blob', mimeType: 'application/epub+zip' });
  const newSize = newBlob.size;
  const timeElapsed = (Date.now() - startTime) / 1000;

  // Log completion
  log('Conversion complete!', 'success', 'DONE');
  logSummary(originalSize, newSize, timeElapsed);

  // Auto-export only if NOT in batch mode (batch mode exports at the end)
  if (!isBatchMode && exportLogCheckbox && exportLogCheckbox.checked) {
    setTimeout(() => {
      exportLogToFile(null, false); // isBatch = false for single file
    }, 100);
  }

  return newBlob;
}

