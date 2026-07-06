#include "LuaAppDisplay.h"

#include "LuaHostApiContext.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"

std::vector<LuaAppDisplayEntry> LuaAppDisplay::entries_;
std::vector<LuaAppBitmapEntry> LuaAppDisplay::bitmapEntries_;
std::vector<LuaAppFillRectEntry> LuaAppDisplay::fillRects_;
std::vector<LuaAppRectEntry> LuaAppDisplay::rects_;
std::vector<LuaAppLineEntry> LuaAppDisplay::lines_;
LuaAppGridState LuaAppDisplay::grid_;
LuaAppListState LuaAppDisplay::list_;
LuaAppPopupState LuaAppDisplay::popup_;
LuaAppChromeState LuaAppDisplay::chrome_;

void LuaAppDisplay::clear() {
  entries_.clear();
  bitmapEntries_.clear();
  fillRects_.clear();
  rects_.clear();
  lines_.clear();
  grid_ = {};
  list_ = {};
  popup_ = {};
  chrome_ = {};
}

bool LuaAppDisplay::addText(const int x, const int y, const char* text, const bool centered) {
  if (text == nullptr) {
    LOG_DBG("APPS", "display addText rejected: null text");
    return false;
  }

  const size_t len = strnlen(text, kMaxTextLen);
  if (len == 0) {
    return true;
  }

  if (entries_.size() >= kMaxEntries) {
    LOG_DBG("APPS", "display addText rejected: buffer full entries=%u", static_cast<unsigned>(entries_.size()));
    return false;
  }

  LuaAppDisplayEntry entry;
  entry.x = static_cast<int16_t>(x);
  entry.y = static_cast<int16_t>(y);
  entry.centered = centered;
  entry.text.assign(text, len);

  entries_.push_back(std::move(entry));
  return true;
}

bool LuaAppDisplay::addBitmap(const int x, const int y, const char* bundleRelativePath, const int maxW,
                              const int maxH) {
  if (bundleRelativePath == nullptr) {
    return false;
  }

  const size_t len = strnlen(bundleRelativePath, kMaxBundlePathLen);
  if (len == 0) {
    return false;
  }

  if (bitmapEntries_.size() >= kMaxBitmapEntries) {
    LOG_DBG("APPS", "display addBitmap rejected: buffer full");
    return false;
  }

  LuaAppBitmapEntry entry;
  entry.x = static_cast<int16_t>(x);
  entry.y = static_cast<int16_t>(y);
  entry.maxW = static_cast<int16_t>(maxW);
  entry.maxH = static_cast<int16_t>(maxH);
  entry.bundleRelativePath.assign(bundleRelativePath, len);

  bitmapEntries_.push_back(std::move(entry));
  return true;
}

bool LuaAppDisplay::addFillRect(const int x, const int y, const int w, const int h) {
  if (fillRects_.size() >= kMaxShapeEntries) {
    return false;
  }
  fillRects_.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                        static_cast<int16_t>(h)});
  return true;
}

bool LuaAppDisplay::addRect(const int x, const int y, const int w, const int h, const int lineWidth) {
  if (rects_.size() >= kMaxShapeEntries) {
    return false;
  }
  rects_.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h),
                    static_cast<int16_t>(lineWidth > 0 ? lineWidth : 1)});
  return true;
}

bool LuaAppDisplay::addLine(const int x1, const int y1, const int x2, const int y2, const int lineWidth) {
  if (lines_.size() >= kMaxShapeEntries) {
    return false;
  }
  lines_.push_back({static_cast<int16_t>(x1), static_cast<int16_t>(y1), static_cast<int16_t>(x2),
                    static_cast<int16_t>(y2), static_cast<int16_t>(lineWidth > 0 ? lineWidth : 1)});
  return true;
}

void LuaAppDisplay::setGrid(const LuaAppGridState& grid) { grid_ = grid; }

void LuaAppDisplay::setList(const LuaAppListState& list) { list_ = list; }

void LuaAppDisplay::setPopup(const char* message) {
  popup_.active = message != nullptr && message[0] != '\0';
  if (popup_.active) {
    const size_t len = strnlen(message, kMaxPopupLen);
    popup_.message.assign(message, len);
  } else {
    popup_.message.clear();
  }
}

void LuaAppDisplay::setChrome(const LuaAppChromeState& chrome) { chrome_ = chrome; }

const std::vector<LuaAppDisplayEntry>& LuaAppDisplay::entries() { return entries_; }

const std::vector<LuaAppBitmapEntry>& LuaAppDisplay::bitmapEntries() { return bitmapEntries_; }

void LuaAppDisplay::paintGrid(GfxRenderer& renderer, const int fontId, const int contentTop) {
  if (!grid_.active || grid_.cell <= 0) {
    return;
  }

  const int ox = grid_.ox;
  const int oy = contentTop + grid_.oy;
  const int cell = grid_.cell;
  const int gridPx = cell * 9;

  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      const int cx = ox + c * cell;
      const int cy = oy + r * cell;
      const bool isCursor = (r + 1) == grid_.cursorR && (c + 1) == grid_.cursorC;
      const bool isConflict = grid_.conflicts[r][c];
      const bool isGiven = grid_.givens[r][c];

      if (isConflict) {
        renderer.fillRect(cx + 1, cy + 1, cell - 2, cell - 2, true);
      } else if (isGiven) {
        renderer.fillRectDither(cx + 1, cy + 1, cell - 2, cell - 2, Color::LightGray);
      } else if (isCursor && grid_.entryMode) {
        renderer.fillRectDither(cx + 1, cy + 1, cell - 2, cell - 2, Color::DarkGray);
      }
    }
  }

  for (int i = 0; i <= 9; ++i) {
    const int lw = (i == 3 || i == 6) ? 2 : 1;
    const int vx = ox + i * cell;
    const int hy = oy + i * cell;
    renderer.drawLine(vx, oy, vx, oy + gridPx, lw, true);
    renderer.drawLine(ox, hy, ox + gridPx, hy, lw, true);
  }

  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      const int cx = ox + c * cell;
      const int cy = oy + r * cell;
      const bool isCursor = (r + 1) == grid_.cursorR && (c + 1) == grid_.cursorC;
      const bool isConflict = grid_.conflicts[r][c];
      const uint8_t digit = grid_.board[r][c];

      if (isCursor) {
        renderer.drawRect(cx + 1, cy + 1, cell - 2, cell - 2, 2, true);
      }

      int showDigit = digit;
      if (isCursor && grid_.entryMode && grid_.entryDigit > 0) {
        showDigit = static_cast<uint8_t>(grid_.entryDigit);
      }

      if (showDigit > 0) {
        char buf[2];
        snprintf(buf, sizeof(buf), "%d", showDigit);
        const auto style = grid_.givens[r][c] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        const int tw = renderer.getTextWidth(fontId, buf, style);
        const int th = renderer.getLineHeight(fontId);
        const int tx = cx + (cell - tw) / 2;
        const int ty = cy + (cell - th) / 2;
        renderer.drawText(fontId, tx, ty, buf, !isConflict, style);
      }
    }
  }
}

void LuaAppDisplay::paintList(GfxRenderer& renderer, const int fontId, const int contentTop, const int contentHeight,
                              const int pageWidth) {
  (void)fontId;
  if (!list_.active || list_.items.empty()) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int selectedIndex = std::max(0, list_.selected - 1);
  const int itemCount = static_cast<int>(list_.items.size());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
      [&](const int index) -> std::string {
        if (index < 0 || index >= itemCount) {
          return "";
        }
        return list_.items[static_cast<size_t>(index)];
      },
      nullptr, nullptr, nullptr, false, nullptr);
}

void LuaAppDisplay::paintPopup(GfxRenderer& renderer, const int fontId) {
  (void)fontId;
  if (!popup_.active || popup_.message.empty()) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily =
      metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, popup_.message.c_str(), popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  if (metrics.popupCornerRadius > 0) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, popup_.message.c_str(), metrics.popupTextInverted, popupFontFamily);
}

void LuaAppDisplay::paint(GfxRenderer& renderer, const int fontId, const std::string& appId, const int contentTop) {
  for (const LuaAppBitmapEntry& bmpEntry : bitmapEntries_) {
    std::string absolutePath;
    if (!buildAppBundlePath(appId, bmpEntry.bundleRelativePath, absolutePath)) {
      LOG_ERR("APPS", "display bmp rejected: invalid path %s", bmpEntry.bundleRelativePath.c_str());
      continue;
    }

    HalFile file;
    if (!Storage.openFileForRead("APPS", absolutePath.c_str(), file)) {
      LOG_ERR("APPS", "display bmp missing: %s", absolutePath.c_str());
      continue;
    }

    Bitmap bitmap(file);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      LOG_ERR("APPS", "display bmp parse failed: %s", absolutePath.c_str());
      continue;
    }

    const int drawY = contentTop + bmpEntry.y;
    const int maxW = bmpEntry.maxW > 0 ? bmpEntry.maxW : bitmap.getWidth();
    const int maxH = bmpEntry.maxH > 0 ? bmpEntry.maxH : bitmap.getHeight();
    renderer.drawBitmap(bitmap, bmpEntry.x, drawY, maxW, maxH);
  }

  for (const LuaAppFillRectEntry& rect : fillRects_) {
    renderer.fillRect(rect.x, contentTop + rect.y, rect.w, rect.h, true);
  }

  paintGrid(renderer, fontId, contentTop);

  for (const LuaAppLineEntry& line : lines_) {
    renderer.drawLine(line.x1, contentTop + line.y1, line.x2, contentTop + line.y2, line.lineWidth, true);
  }

  for (const LuaAppRectEntry& rect : rects_) {
    renderer.drawRect(rect.x, contentTop + rect.y, rect.w, rect.h, rect.lineWidth, true);
  }

  for (const LuaAppDisplayEntry& entry : entries_) {
    const int drawY = contentTop + entry.y;
    if (entry.centered) {
      renderer.drawCenteredText(fontId, drawY, entry.text.c_str());
    } else {
      renderer.drawText(fontId, entry.x, drawY, entry.text.c_str());
    }
  }
}

void LuaAppDisplay::paintWithChrome(GfxRenderer& renderer, const int fontId, const std::string& appId,
                                    const std::string& displayName, const int contentTop, const int contentHeight) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const char* subtitle = chrome_.subtitle.empty() ? nullptr : chrome_.subtitle.c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, displayName.c_str(), subtitle);

  paint(renderer, fontId, appId, contentTop);
  paintList(renderer, fontId, contentTop, contentHeight, pageWidth);
  paintPopup(renderer, fontId);

  const char* hintBack = chrome_.hasHints ? chrome_.hintBack.c_str() : "";
  const char* hintConfirm = chrome_.hasHints ? chrome_.hintConfirm.c_str() : "";
  const char* hintLeft = chrome_.hasHints ? chrome_.hintLeft.c_str() : "";
  const char* hintRight = chrome_.hasHints ? chrome_.hintRight.c_str() : "";
  GUI.drawButtonHints(renderer, hintBack, hintConfirm, hintLeft, hintRight);

  (void)pageHeight;
  renderer.displayBuffer();
}
