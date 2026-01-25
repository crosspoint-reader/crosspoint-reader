#include "LayoutElements.h"

#include <Bitmap.h>

#include "ThemeManager.h"

namespace ThemeEngine {

// Built-in icon drawing
// These are simple geometric representations of common icons
void Icon::draw(const GfxRenderer& renderer, const ThemeContext& context) {
  if (!isVisible(context)) {
    markClean();
    return;
  }

  std::string iconName = context.evaluatestring(srcExpr);
  if (iconName.empty()) {
    markClean();
    return;
  }

  std::string colStr = context.evaluatestring(colorExpr);
  uint8_t color = Color::parse(colStr).value;
  bool black = (color == 0x00);

  // Use absW/absH if set, otherwise use iconSize
  int w = absW > 0 ? absW : iconSize;
  int h = absH > 0 ? absH : iconSize;
  int cx = absX + w / 2;
  int cy = absY + h / 2;

  // Check if it's a path to a BMP file
  if (iconName.find('/') != std::string::npos || iconName.find('.') != std::string::npos) {
    // Try to load as bitmap
    std::string path = iconName;
    if (path[0] != '/') {
      path = ThemeManager::get().getAssetPath(iconName);
    }

    const std::vector<uint8_t>* data = ThemeManager::get().getCachedAsset(path);
    if (data && !data->empty()) {
      Bitmap bmp(data->data(), data->size());
      if (bmp.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bmp, absX, absY, w, h);
        markClean();
        return;
      }
    }
  }

  // Built-in icons (simple geometric shapes)
  if (iconName == "heart" || iconName == "favorite") {
    // Simple heart shape approximation
    int s = w / 4;
    renderer.fillRect(cx - s, cy - s / 2, s * 2, s, black);
    renderer.fillRect(cx - s * 3 / 2, cy - s, s, s, black);
    renderer.fillRect(cx + s / 2, cy - s, s, s, black);
    // Bottom point
    for (int i = 0; i < s; i++) {
      renderer.drawLine(cx - s + i, cy + i, cx + s - i, cy + i, black);
    }
  } else if (iconName == "book" || iconName == "books") {
    // Book icon
    int bw = w * 2 / 3;
    int bh = h * 3 / 4;
    int bx = absX + (w - bw) / 2;
    int by = absY + (h - bh) / 2;
    renderer.drawRect(bx, by, bw, bh, black);
    renderer.drawLine(bx + bw / 3, by, bx + bw / 3, by + bh - 1, black);
    // Pages
    renderer.drawLine(bx + 2, by + bh / 4, bx + bw / 3 - 2, by + bh / 4, black);
    renderer.drawLine(bx + 2, by + bh / 2, bx + bw / 3 - 2, by + bh / 2, black);
  } else if (iconName == "folder" || iconName == "files") {
    // Folder icon
    int fw = w * 3 / 4;
    int fh = h * 2 / 3;
    int fx = absX + (w - fw) / 2;
    int fy = absY + (h - fh) / 2;
    // Tab
    renderer.fillRect(fx, fy, fw / 3, fh / 6, black);
    // Body
    renderer.drawRect(fx, fy + fh / 6, fw, fh - fh / 6, black);
  } else if (iconName == "settings" || iconName == "gear") {
    // Gear icon - simplified as circle with notches
    int r = w / 3;
    // Draw circle approximation
    renderer.drawRect(cx - r, cy - r, r * 2, r * 2, black);
    // Inner circle
    int ir = r / 2;
    renderer.drawRect(cx - ir, cy - ir, ir * 2, ir * 2, black);
    // Teeth
    int t = r / 3;
    renderer.fillRect(cx - t / 2, absY, t, r - ir, black);
    renderer.fillRect(cx - t / 2, cy + r, t, r - ir, black);
    renderer.fillRect(absX, cy - t / 2, r - ir, t, black);
    renderer.fillRect(cx + r, cy - t / 2, r - ir, t, black);
  } else if (iconName == "transfer" || iconName == "arrow" || iconName == "send") {
    // Arrow pointing right
    int aw = w / 2;
    int ah = h / 3;
    int ax = absX + w / 4;
    int ay = cy - ah / 2;
    // Shaft
    renderer.fillRect(ax, ay, aw, ah, black);
    // Arrow head
    for (int i = 0; i < ah; i++) {
      renderer.drawLine(ax + aw, cy - ah + i, ax + aw + ah - i, cy, black);
      renderer.drawLine(ax + aw, cy + ah - i, ax + aw + ah - i, cy, black);
    }
  } else if (iconName == "library" || iconName == "device") {
    // Device/tablet icon
    int dw = w * 2 / 3;
    int dh = h * 3 / 4;
    int dx = absX + (w - dw) / 2;
    int dy = absY + (h - dh) / 2;
    renderer.drawRect(dx, dy, dw, dh, black);
    // Screen
    renderer.drawRect(dx + 2, dy + 2, dw - 4, dh - 8, black);
    // Home button
    renderer.fillRect(dx + dw / 2 - 2, dy + dh - 5, 4, 2, black);
  } else if (iconName == "battery") {
    // Battery icon
    int bw = w * 3 / 4;
    int bh = h / 2;
    int bx = absX + (w - bw) / 2;
    int by = absY + (h - bh) / 2;
    renderer.drawRect(bx, by, bw - 3, bh, black);
    renderer.fillRect(bx + bw - 3, by + bh / 4, 3, bh / 2, black);
  } else if (iconName == "check" || iconName == "checkmark") {
    // Checkmark
    int x1 = absX + w / 4;
    int y1 = cy;
    int x2 = cx;
    int y2 = absY + h * 3 / 4;
    int x3 = absX + w * 3 / 4;
    int y3 = absY + h / 4;
    renderer.drawLine(x1, y1, x2, y2, black);
    renderer.drawLine(x2, y2, x3, y3, black);
    // Thicken
    renderer.drawLine(x1, y1 + 1, x2, y2 + 1, black);
    renderer.drawLine(x2, y2 + 1, x3, y3 + 1, black);
  } else if (iconName == "back" || iconName == "left") {
    // Left arrow
    int s = w / 3;
    for (int i = 0; i < s; i++) {
      renderer.drawLine(cx - s + i, cy, cx, cy - s + i, black);
      renderer.drawLine(cx - s + i, cy, cx, cy + s - i, black);
    }
  } else if (iconName == "up") {
    // Up arrow
    int s = h / 3;
    for (int i = 0; i < s; i++) {
      renderer.drawLine(cx, cy - s + i, cx - s + i, cy, black);
      renderer.drawLine(cx, cy - s + i, cx + s - i, cy, black);
    }
  } else if (iconName == "down") {
    // Down arrow
    int s = h / 3;
    for (int i = 0; i < s; i++) {
      renderer.drawLine(cx, cy + s - i, cx - s + i, cy, black);
      renderer.drawLine(cx, cy + s - i, cx + s - i, cy, black);
    }
  } else {
    // Unknown icon - draw placeholder
    renderer.drawRect(absX, absY, w, h, black);
    renderer.drawLine(absX, absY, absX + w - 1, absY + h - 1, black);
    renderer.drawLine(absX + w - 1, absY, absX, absY + h - 1, black);
  }

  markClean();
}

}  // namespace ThemeEngine
