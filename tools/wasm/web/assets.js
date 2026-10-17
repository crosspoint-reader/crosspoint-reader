// Standalone pages provide their assets once, shared with each fresh srcdoc frame.
export const embedded = globalThis.crosspointBundle ?? null;
export const messageOrigin = embedded?.origin ?? location.origin;
// file:// messages have an opaque origin; callers also check the exact peer window.
export const targetOrigin = messageOrigin === 'null' ? '*' : messageOrigin;

function decode(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

export function firmwareOptions(device) {
  if (embedded) {
    if (device !== embedded.device) throw new Error('Device is not included in this file');
    return {
      wasmBinary: decode(embedded.wasm),
      getPreloadedPackage: () => decode(embedded.data).buffer,
      locateFile: (path) => path,
    };
  }
  return { locateFile: (path) => new URL(`./${device}/${path}`, location.href).href };
}
