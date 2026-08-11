// Framebuffer primitives.

#include "internal.h"

using rust_ffi::asText;
using rust_ffi::resolveStyle;

extern "C" {

int32_t cpp_screen_width() { return g_rustRendererPtr ? g_rustRendererPtr->getScreenWidth() : 0; }

int32_t cpp_screen_height() { return g_rustRendererPtr ? g_rustRendererPtr->getScreenHeight() : 0; }

void cpp_clear_screen() {
  if (g_rustRendererPtr) g_rustRendererPtr->clearScreen();
}

void cpp_draw_text(const int32_t x, const int32_t y, const uint8_t* text, const int32_t fontId, const uint8_t style) {
  if (!g_rustRendererPtr || !text || fontId == 0) return;
  g_rustRendererPtr->drawText(fontId, x, y, asText(text), true, resolveStyle(style));
}

void cpp_draw_rect(const int32_t x, const int32_t y, const int32_t width, const int32_t height, const uint8_t filled,
                   const uint8_t black) {
  if (!g_rustRendererPtr) return;
  if (filled) {
    g_rustRendererPtr->fillRect(x, y, width, height, black != 0);
  } else {
    g_rustRendererPtr->drawRect(x, y, width, height, black != 0);
  }
}

void cpp_draw_line(const int32_t x1, const int32_t y1, const int32_t x2, const int32_t y2) {
  if (g_rustRendererPtr) g_rustRendererPtr->drawLine(x1, y1, x2, y2);
}

void cpp_fill_rect_dither(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                          const uint8_t light) {
  if (!g_rustRendererPtr) return;
  g_rustRendererPtr->fillRectDither(x, y, width, height, light ? Color::LightGray : Color::DarkGray);
}

// Darkens a region while leaving what is already drawn there legible.
//
// fillRectDither cannot do this: fillRectImpl memsets the interior before
// applying the pattern, so it destroys the content underneath. Writing ink on
// one checkerboard parity only leaves the other half of the pixels untouched,
// which reads as grey and keeps roughly half of whatever was behind it.
// BaseTheme.cpp does the same thing in the lightening direction for dimmed rows.
void cpp_set_clip(const int32_t x, const int32_t y, const int32_t width, const int32_t height) {
  if (!g_rustRendererPtr) return;
  // Zero-sized means "no clip", so the boundary needs one function, not two.
  if (width <= 0 || height <= 0) {
    g_rustRendererPtr->clearClip();
    return;
  }
  g_rustRendererPtr->setClip(x, y, width, height);
}

void cpp_scrim(const int32_t x, const int32_t y, const int32_t width, const int32_t height) {
  if (!g_rustRendererPtr || width <= 0 || height <= 0) return;

  // Clip here rather than leaning on drawPixel: it LOG_ERRs on every pixel it
  // rejects, so an oversized rect would emit thousands of error lines.
  const int32_t right = g_rustRendererPtr->getScreenWidth();
  const int32_t bottom = g_rustRendererPtr->getScreenHeight();
  const int32_t x0 = x > 0 ? x : 0;
  const int32_t y0 = y > 0 ? y : 0;
  const int32_t x1 = (x + width) < right ? (x + width) : right;
  const int32_t y1 = (y + height) < bottom ? (y + height) : bottom;

  for (int32_t py = y0; py < y1; ++py) {
    // Start on the first column whose parity matches this row, then stride two,
    // so the branch per pixel disappears and only half the pixels are visited.
    for (int32_t px = x0 + ((x0 + py) & 1); px < x1; px += 2) {
      g_rustRendererPtr->drawPixel(px, py, true);
    }
  }
}

void cpp_draw_image(const uint8_t* bitmap, const int32_t x, const int32_t y, const int32_t width,
                    const int32_t height) {
  if (!g_rustRendererPtr || !bitmap) return;
  g_rustRendererPtr->drawImage(bitmap, x, y, width, height);
}
}
