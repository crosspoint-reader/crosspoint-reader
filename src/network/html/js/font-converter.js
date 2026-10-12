// The Font Manager's TTF/OTF converter, run as a module Web Worker so the work does not freeze the
// page. FreeType (WebAssembly) and the .cpfont writer come from the bundle beside this file; they
// are tools/ftwasm, which produces the same files as lib/EpdFont/scripts/fontconvert_sdcard.py.
//
// In:  { fonts: { regular, bold, italic, bolditalic } as ArrayBuffers (regular or any one needed),
//        name, sizes: [10, 12, ...], intervals: 'builtin,latin-cyrillic,...', fileNames: {...} }
// Out: { type: 'progress', done, total, size, style }
//      { type: 'done', files: [{ fileName, bytes, styles }] }   (bytes: Uint8Array, transferred)
//      { type: 'error', message }
import createFtWasm from './ftwasm.mjs';
import { FreeTypeWasm } from './cpfont-raster.mjs';
import { convertFamily } from './cpfont-writer.mjs';

let ft = null;

self.onmessage = async (event) => {
  try {
    if (!ft) ft = await FreeTypeWasm.create(createFtWasm);
    const job = event.data;
    const fonts = {};
    for (const [style, buffer] of Object.entries(job.fonts)) {
      if (buffer) fonts[style] = new Uint8Array(buffer);
    }
    const files = convertFamily(ft, {
      fonts,
      name: job.name,
      sizes: job.sizes,
      intervals: job.intervals,
      fileNames: job.fileNames,
      onProgress: (p) => self.postMessage({ type: 'progress', done: p.done, total: p.total, size: p.size, style: p.style })
    });
    self.postMessage(
      { type: 'done', files: files.map((f) => ({ fileName: f.fileName, bytes: f.bytes, styles: f.styles })) },
      files.map((f) => f.bytes.buffer));
  } catch (err) {
    self.postMessage({ type: 'error', message: (err && err.message) || String(err) });
  }
};
