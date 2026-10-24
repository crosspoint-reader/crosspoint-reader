import { createSdCard } from './sd-card.js';
import { devices, keyMap } from './devices.js';
import { embedded, messageOrigin, targetOrigin, firmwareOptions } from './assets.js';

const canvas = document.querySelector('#display');
const context = canvas.getContext('2d');
const send = (type, extra = {}) => parent.postMessage({ type, ...extra }, targetOrigin);
let module;
let sdCard;
let initialized = false;
let firstFrame = false;
let bootTimer;
let animation;
let taskTimer;
let disposed = false;
let profile;
let pointerId = null;
let point = [0, 0];
let mouseStart = null;
const MOUSE_DRAG_DISTANCE = 12; // CSS pixels, independent of display scale/orientation.
const TOUCH_SWIPE_DISTANCE = 72; // Clear the simulator's 60 px threshold after coordinate rounding.
const heldKeys = new Set();

function releaseButtons() {
  heldKeys.clear();
  module?._preview_release_buttons?.();
}
function cancelTouch() {
  if (pointerId === null) return;
  const id = pointerId;
  pointerId = null;
  mouseStart = null;
  module?._preview_touch?.(3, ...point);
  if (canvas.hasPointerCapture(id)) canvas.releasePointerCapture(id);
}
function release() { cancelTouch(); releaseButtons(); }

function coordinates(event) {
  const rect = canvas.getBoundingClientRect();
  return [
    Math.round(Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)) * (canvas.width - 1)),
    Math.round(Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height)) * (canvas.height - 1)),
  ];
}
function mouseSwipeCoordinates(event) {
  if (!mouseStart) return null;
  const dx = event.clientX - mouseStart.clientX;
  const dy = event.clientY - mouseStart.clientY;
  if (Math.max(Math.abs(dx), Math.abs(dy)) < MOUSE_DRAG_DISTANCE) return null;
  const rect = canvas.getBoundingClientRect();
  const width = canvas.width - 1;
  const height = canvas.height - 1;
  let vx = dx * width / rect.width;
  let vy = dy * height / rect.height;
  const scale = Math.min(
    Math.max(1, TOUCH_SWIPE_DISTANCE / Math.max(Math.abs(vx), Math.abs(vy))),
    width / Math.abs(vx), height / Math.abs(vy),
  );
  vx *= scale;
  vy *= scale;
  // Translate the segment into the display instead of clipping its distance
  // away when a flick starts near an edge and ends on the bezel.
  const sx = Math.max(Math.max(0, -vx), Math.min(width - Math.max(0, vx), mouseStart.point[0]));
  const sy = Math.max(Math.max(0, -vy), Math.min(height - Math.max(0, vy), mouseStart.point[1]));
  return [sx, sy, sx + vx, sy + vy].map(Math.round);
}
canvas.addEventListener('pointerdown', (event) => {
  if (!profile?.touch || !firstFrame || pointerId !== null || !event.isPrimary || event.button !== 0) return;
  event.preventDefault();
  canvas.focus({ preventScroll: true });
  pointerId = event.pointerId;
  point = coordinates(event);
  mouseStart = event.pointerType === 'mouse' ? { point, clientX: event.clientX, clientY: event.clientY } : null;
  canvas.setPointerCapture(pointerId);
  module._preview_touch(0, ...point);
});
canvas.addEventListener('pointermove', (event) => {
  if (pointerId !== event.pointerId) return;
  if (!canvas.hasPointerCapture(pointerId)) { cancelTouch(); return; }
  event.preventDefault();
  point = coordinates(event);
  module._preview_touch(1, ...point);
});
function finishTouch(event, releasedByBrowser = false) {
  if (pointerId !== event.pointerId) return;
  event.preventDefault();
  // Capture released before its first move may never emit lostpointercapture.
  if (!releasedByBrowser && !canvas.hasPointerCapture(pointerId)) { cancelTouch(); return; }
  const end = coordinates(event);
  const mouseSwipe = mouseSwipeCoordinates(event);
  const rect = canvas.getBoundingClientRect();
  const outside = event.clientX < rect.left || event.clientX >= rect.right ||
    event.clientY < rect.top || event.clientY >= rect.bottom;
  if (outside && !mouseSwipe) {
    cancelTouch();
    return;
  }
  if (mouseSwipe) {
    // Publish the entire contact replacement at once on the firmware queue.
    module._preview_mouse_swipe(...mouseSwipe);
  } else {
    module._preview_touch(2, ...end);
  }
  pointerId = null;
  mouseStart = null;
  point = end;
  if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
}
canvas.addEventListener('pointerup', (event) => finishTouch(event));
canvas.addEventListener('pointercancel', (event) => { if (pointerId === event.pointerId) cancelTouch(); });
canvas.addEventListener('lostpointercapture', (event) => {
  if (pointerId !== event.pointerId) return;
  // Chrome may release capture for a final mouse move with buttons=0 before
  // delivering pointerup. Complete that release once, using its final position.
  if (mouseStart && event.buttons === 0 && event.button === -1) finishTouch(event, true);
  else cancelTouch();
});
window.disposePreview = () => {
  disposed = true;
  clearTimeout(bootTimer);
  cancelAnimationFrame(animation);
  clearTimeout(taskTimer);
  module?._preview_stop?.();
};
window.addEventListener('pagehide', window.disposePreview);
window.addEventListener('blur', release);
document.addEventListener('visibilitychange', () => { if (document.hidden) release(); });
for (const [event, down] of [['keydown', 1], ['keyup', 0]]) {
  window.addEventListener(event, (e) => {
    const button = keyMap[e.key];
    if (!profile?.buttons.includes(button)) return;
    e.preventDefault();
    if (e.repeat) return;
    if (down) heldKeys.add(button); else heldKeys.delete(button);
    module?._preview_button?.(button, down);
  });
}

function fail(error) {
  if (disposed) return;
  clearTimeout(bootTimer);
  send('log', { text: String(error?.stack || error) });
  send('failed');
}
window.addEventListener('error', (e) => fail(e.error || e.message));
window.addEventListener('unhandledrejection', (e) => fail(e.reason));

async function start({ device, entries }) {
  if (!Object.hasOwn(devices, device)) throw new Error('Unknown device');
  profile = devices[device];
  canvas.classList.toggle('touch', profile.touch);
  const { default: createCrosspoint } = await import(embedded?.firmware ?? `./${device}/firmware.js`);
  if (disposed) return;
  module = {
    ...firmwareOptions(device),
    print: (text) => send('log', { text }),
    printErr: (text) => send('log', { text }),
    onAbort: (reason) => fail(reason),
  };
  await createCrosspoint(module);
  if (disposed) return;
  module.FS.chdir('/');
  sdCard = createSdCard(module);
  if (entries) sdCard({ operation: 'restore', entries });
  send('booting');
  if (!module._preview_start()) throw new Error('Firmware task did not start');
  function step() {
    if (disposed) return;
    try {
      taskTimer = setTimeout(step, module._preview_step());
    } catch (error) {
      fail(error);
    }
  }
  taskTimer = setTimeout(step, 0);
  bootTimer = setTimeout(() => { if (!firstFrame) send('timeout'); }, 30000);
  const width = module._preview_width();
  const height = module._preview_height();
  const offscreen = document.createElement('canvas');
  offscreen.width = width;
  offscreen.height = height;
  const offContext = offscreen.getContext('2d');
  const image = offContext.createImageData(width, height);
  function paint() {
    if (disposed) return;
    const ptr = module._preview_take_frame();
    if (ptr) {
      const heap = module.HEAPU32;
      for (let i = 0; i < width * height; ++i) {
        const argb = heap[(ptr >>> 2) + i];
        image.data[i * 4] = (argb >>> 16) & 255;
        image.data[i * 4 + 1] = (argb >>> 8) & 255;
        image.data[i * 4 + 2] = argb & 255;
        image.data[i * 4 + 3] = 255;
      }
      offContext.putImageData(image, 0, 0);
      const rotation = module._preview_rotation();
      if (canvas.dataset.rotation !== String(rotation)) cancelTouch();
      canvas.dataset.rotation = rotation;
      const portrait = rotation === 90 || rotation === 270;
      const w = portrait ? height : width;
      const h = portrait ? width : height;
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        send('geometry', { width: w, height: h });
      }
      context.save();
      context.translate(w / 2, h / 2);
      context.rotate(rotation * Math.PI / 180);
      context.drawImage(offscreen, -width / 2, -height / 2);
      context.restore();
      if (!firstFrame) {
        firstFrame = true;
        clearTimeout(bootTimer);
        send('running');
      }
    }
    animation = requestAnimationFrame(paint);
  }
  animation = requestAnimationFrame(paint);
}

window.addEventListener('message', (event) => {
  if (event.source !== parent || event.origin !== messageOrigin) return;
  if (disposed) return;
  const data = event.data;
  if (data.type === 'init' && !initialized) {
    initialized = true;
    start(data).catch(fail);
  } else if (data.type === 'sd-request') {
    try {
      if (!firstFrame || !sdCard) throw { code: 'notReady' };
      if (!['list', 'download', 'snapshot', 'mkdir', 'write', 'rename', 'delete'].includes(data.operation)) throw { code: 'invalidOperation' };
      const result = sdCard(data);
      const buffers = result.bytes ? [result.bytes] : result.entries?.filter(entry => entry.bytes).map(entry => entry.bytes) ?? [];
      parent.postMessage({ type: 'sd-response', id: data.id, result }, targetOrigin, buffers);
    } catch (error) {
      if (!error.code || error.errno) send('log', { text: `SD ${data.operation}: ${error.stack || error}` });
      send('sd-response', { id: data.id, error: { code: error.errno ? 'fileError' : error.code || 'fileError', path: error.path || data.path || '' } });
    }
  } else if (data.type === 'button') {
    if (profile?.buttons.includes(data.button)) module?._preview_button?.(data.button, data.down);
  } else if (data.type === 'release') {
    // Parent blur can mean focus just entered this canvas; preserve that touch.
    releaseButtons();
  } else if (data.type === 'screenshot' && firstFrame) {
    canvas.toBlob((blob) => send('screenshot', { blob }), 'image/png');
  }
});
send('frame-ready');
