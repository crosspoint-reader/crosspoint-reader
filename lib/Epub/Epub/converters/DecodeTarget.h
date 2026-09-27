#pragma once

#include <stdint.h>

// Value snapshot of the GfxRenderer state an image decode needs. The idle
// prefetch cacher runs on its own task and must never touch the live renderer
// (the render task owns it), so it decodes from a captured target instead. The
// framebuffer-writing decode path captures the live renderer the same way, so
// both share one decoder code path.
//
// renderMode/orientation are held as raw bytes (the GfxRenderer enum values) so
// this header stays free of the renderer dependency: host unit tests compile
// Page/ImageBlock without GfxRenderer.h.
class GfxRenderer;

struct DecodeTarget {
  bool writeFramebuffer = true;  // false = cache-only decode, framebuffer untouched
  uint8_t* framebuffer = nullptr;
  int writeOriginY = 0;
  int writeRows = 0;
  uint8_t renderMode = 0;   // GfxRenderer::RenderMode
  uint8_t orientation = 0;  // GfxRenderer::Orientation
  bool grayAbsolute = false;
  int displayWidthBytes = 0;
  int displayWidth = 0;
  int displayHeight = 0;
  int screenWidth = 0;
  int screenHeight = 0;
};

// Capture the live renderer's state. Must be called while the caller holds the
// render lock (normal decode) or on the loop task (prefetch, before handing the
// target to the cacher).
DecodeTarget captureDecodeTarget(const GfxRenderer& renderer, bool writeFramebuffer = true);
