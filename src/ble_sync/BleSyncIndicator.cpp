#include "BleSyncIndicator.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "BleSyncManager.h"
#include "fontIds.h"

namespace {
// Battery in the header: rect at (…, header.y + 14), 15x12, drawn with an
// internal +6 → its icon centerline sits at header.y + 20. Match that band so the
// sync glyph lines up with the battery and is the same visual size.
constexpr int kBattTopOffset = 14;                // theme's battery rect top within the header
constexpr int kBattCenterY = kBattTopOffset + 6;  // battery icon centerline (from header.y)
constexpr int kLeftPad = 8;                       // left inset (mirrors the battery's right inset)
constexpr int kR = 7;                             // glyph radius (~battery 15x12)
constexpr int kStroke = 2;                        // ring/line thickness

// The four quadrants as (xDir, yDir) sign pairs, in clockwise order starting
// top-right. Omitting one gives a rotating "C" spinner.
constexpr int kQuad[4][2] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}};

void ring(const GfxRenderer& r, int cx, int cy, int skip /* -1 = full ring */) {
  for (int q = 0; q < 4; ++q) {
    if (q == skip) continue;
    r.drawArc(kR, cx, cy, kQuad[q][0], kQuad[q][1], kStroke, true);
  }
}

void checkmark(const GfxRenderer& r, int cx, int cy) {
  r.drawLine(cx - 4, cy, cx - 1, cy + 4, kStroke, true);
  r.drawLine(cx - 1, cy + 4, cx + 4, cy - 4, kStroke, true);
}

void cross(const GfxRenderer& r, int cx, int cy) {
  r.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, kStroke, true);
  r.drawLine(cx + 4, cy - 4, cx - 4, cy + 4, kStroke, true);
}
}  // namespace

namespace BleSync {

bool indicatorVisible() { return BLE_SYNC.isActive() || BLE_SYNC.hasFreshResult(); }

bool drawIndicator(GfxRenderer& renderer, int headerX, int headerY, int headerW, int headerH) {
  (void)headerW;
  (void)headerH;
  if (!indicatorVisible()) return false;

  const Status s = BLE_SYNC.status();
  const int cy = headerY + kBattCenterY;  // align vertically with the battery icon
  const int cx = headerX + kLeftPad + kR;

  std::string label;
  switch (s.phase) {
    case Phase::Advertising:
      ring(renderer, cx, cy, s.spin & 0x03);  // spinning C
      label = tr(STR_BLE_SYNCING);
      break;
    case Phase::Syncing:
      ring(renderer, cx, cy, s.spin & 0x03);
      if (s.bookCount > 0) {
        char b[16];
        std::snprintf(b, sizeof(b), tr(STR_BLE_SYNC_COUNT_FORMAT), s.bookIndex, s.bookCount);
        label = b;
      } else {
        label = tr(STR_BLE_SYNCING);
      }
      break;
    case Phase::Success:
      ring(renderer, cx, cy, -1);
      checkmark(renderer, cx, cy);
      label = tr(STR_BLE_SYNCED);
      break;
    case Phase::Failed:
      ring(renderer, cx, cy, -1);
      cross(renderer, cx, cy);
      label = tr(STR_SYNC_FAILED_MSG);
      break;
    default:
      return false;
  }

  const int textX = cx + kR + 5;
  const int textY = cy - renderer.getLineHeight(SMALL_FONT_ID) / 2;  // battery-% font, centered on the glyph
  renderer.drawText(SMALL_FONT_ID, textX, textY, label.c_str(), true);
  return true;
}

}  // namespace BleSync
