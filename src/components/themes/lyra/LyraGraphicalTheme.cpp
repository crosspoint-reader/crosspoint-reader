#include "LyraGraphicalTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {

// Grid layout
constexpr int GRID_COLS = 2;
constexpr int TILE_HEIGHT = 120;
constexpr int TILE_GAP = 12;
constexpr int ICON_SIZE = 32;

constexpr int CARD_RADIUS = 10;
constexpr int CARD_PADDING = 12;

// Copiado del patrón de LyraTheme.cpp (mapeo de iconos existente)
const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

std::string LyraGraphicalTheme::basename(const std::string& path) {
  auto pos = path.find_last_of('/');
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string LyraGraphicalTheme::fileExtLower(const std::string& path) {
  auto pos = path.find_last_of('.');
  if (pos == std::string::npos) return {};
  return toLower(path.substr(pos + 1));
}
void LyraGraphicalTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                        const std::function<std::string(int)>& buttonLabel,
                                        const std::function<UIIcon(int)>& rowIcon) const {
  const int sidePad = LyraMetrics::values.contentSidePadding;
  const int x0 = rect.x + sidePad;
  const int w0 = rect.width - sidePad * 2;

  const int tileW = (w0 - TILE_GAP) / GRID_COLS;

  for (int i = 0; i < buttonCount; ++i) {
    const int row = i / GRID_COLS;
    const int col = i % GRID_COLS;

    const int x = x0 + col * (tileW + TILE_GAP);
    const int y = rect.y + row * (TILE_HEIGHT + TILE_GAP);

    const bool selected = (i == selectedIndex);

    if (selected) {
      renderer.fillRoundedRect(x, y, tileW, TILE_HEIGHT, CARD_RADIUS, Color::LightGray);
    } else {
      renderer.fillRoundedRect(x, y, tileW, TILE_HEIGHT, CARD_RADIUS, Color::White);
    }
    renderer.drawRoundedRect(x, y, tileW, TILE_HEIGHT, 1, CARD_RADIUS, true);

    // Icon
    const UIIcon icon = rowIcon ? rowIcon(i) : UIIcon::File;
    const uint8_t* bmp = iconForName(icon, ICON_SIZE);
    if (bmp) {
      renderer.drawIcon(bmp, x + (tileW - ICON_SIZE) / 2, y + CARD_PADDING, ICON_SIZE, ICON_SIZE);
    }

    // Label
    const std::string label = buttonLabel ? buttonLabel(i) : "";
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::REGULAR);

    renderer.drawText(UI_12_FONT_ID, x + (tileW - tw) / 2, y + CARD_PADDING + ICON_SIZE + 8, label.c_str(), true);
  }
}
void LyraGraphicalTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                             const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                             bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                             std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;

  const int sidePad = LyraMetrics::values.contentSidePadding;
  const int x = rect.x + sidePad;
  const int y = rect.y;
  const int w = rect.width - sidePad * 2;
  const int h = rect.height;

  const bool selected = (selectorIndex == 0);

  if (selected) {
    renderer.fillRoundedRect(x, y, w, h, CARD_RADIUS, Color::LightGray);
  } else {
    renderer.fillRoundedRect(x, y, w, h, CARD_RADIUS, Color::White);
  }
  renderer.drawRoundedRect(x, y, w, h, 1, CARD_RADIUS, true);

  renderer.drawText(UI_10_FONT_ID, x + 12, y + 8, tr(STR_CONTINUE_READING), true);

  if (recentBooks.empty()) {
    renderer.drawText(UI_12_FONT_ID, x + 12, y + h / 2, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    return;
  }

  const RecentBook& book = recentBooks.front();

  // Cover (mismo enfoque que LyraTheme: cargar BMP thumb si existe)
  const int coverH = LyraMetrics::values.homeCoverHeight;
  const int coverW = static_cast<int>(coverH * 0.6f);

  const int coverX = x + 12;
  const int coverY = y + 28;

  if (!coverRendered) {
    bool hasCover = !book.coverBmpPath.empty();

    if (hasCover) {
      const std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverH);
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, coverX, coverY, coverW, coverH);
        } else {
          hasCover = false;
        }
        file.close();
      } else {
        hasCover = false;
      }
    }

    // frame
    renderer.drawRect(coverX, coverY, coverW, coverH, true);

    if (!hasCover) {
      // empty cover fallback
      renderer.fillRect(coverX, coverY + (coverH / 3), coverW, 2 * coverH / 3, true);
      renderer.drawIcon(CoverIcon, coverX + 24, coverY + 24, 32, 32);
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = true;
  }

  // Text area
  const int textX = coverX + coverW + 16;
  int textY = coverY + 8;
  const int textW = x + w - textX - 12;

  // Title (bold, trunc)
  const std::string t = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, textY, t.c_str(), true, EpdFontFamily::BOLD);
  textY += renderer.getLineHeight(UI_12_FONT_ID) + 4;

  // Author (regular)
  if (!book.author.empty()) {
    const std::string a = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textW, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, textX, textY, a.c_str(), true);
    textY += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  }

  // Meta: ext + filename
  const std::string ext = fileExtLower(book.path);
  std::string meta = ext.empty() ? basename(book.path) : ("." + ext + " · " + basename(book.path));
  meta = renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), textW, EpdFontFamily::REGULAR);
  renderer.drawText(SMALL_FONT_ID, textX, textY, meta.c_str(), true);
}
