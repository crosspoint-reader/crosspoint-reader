#include "ForkDriftTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {
constexpr int hPadding = 8;
constexpr int cornerRadius = 6;
constexpr int gridCols = 3;
constexpr int gridRows = 2;
constexpr int buttonCols = 2;
constexpr int buttonRows = 2;

const uint8_t* iconFor(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Text:
        return Text24Icon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}
}  // namespace

void ForkDriftTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int pad = ForkDriftMetrics::values.contentSidePadding;
  const int tileWidth = (rect.width - 2 * pad) / gridCols;
  const int bookCount = static_cast<int>(recentBooks.size());
  const int usedRows = bookCount > gridCols ? gridRows : 1;
  const int maxCells = gridCols * usedRows;
  const int tileHeight = rect.height / usedRows;
  const int coverHeight = ForkDriftMetrics::values.homeCoverHeight;
  const bool hasBooks = !recentBooks.empty();

  if (hasBooks) {
    if (!coverRendered) {
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), maxCells); i++) {
        const int col = i % gridCols;
        const int row = i / gridCols;
        const int tileX = pad + col * tileWidth;
        const int tileY = rect.y + row * tileHeight;

        bool hasCover = !recentBooks[i].coverBmpPath.empty();
        if (hasCover) {
          const std::string path = UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, coverHeight);
          FsFile file;
          if (Storage.openFileForRead("HOME", path, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float imgW = static_cast<float>(bitmap.getWidth());
              float imgH = static_cast<float>(bitmap.getHeight());
              float ratio = imgW / imgH;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPadding) / static_cast<float>(coverHeight);
              float cropX = 1.0f - (tileRatio / ratio);
              renderer.drawBitmap(bitmap, tileX + hPadding, tileY + hPadding, tileWidth - 2 * hPadding, coverHeight,
                                  cropX);
            } else {
              hasCover = false;
            }
            file.close();
          } else {
            hasCover = false;
          }
        }

        renderer.drawRect(tileX + hPadding, tileY + hPadding, tileWidth - 2 * hPadding, coverHeight, true);
        if (!hasCover) {
          renderer.fillRect(tileX + hPadding, tileY + hPadding + coverHeight / 3, tileWidth - 2 * hPadding,
                            2 * coverHeight / 3, true);
          renderer.drawIcon(CoverIcon, tileX + hPadding + 16, tileY + hPadding + 16, 32, 32);
        }
      }
      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), maxCells); i++) {
      const int col = i % gridCols;
      const int row = i / gridCols;
      const int tileX = pad + col * tileWidth;
      const int tileY = rect.y + row * tileHeight;
      const bool selected = (selectorIndex == i);

      auto title = renderer.truncatedText(UI_10_FONT_ID, recentBooks[i].title.c_str(), tileWidth - 2 * hPadding);
      int textY = tileY + coverHeight + hPadding + 4;
      renderer.drawText(UI_10_FONT_ID, tileX + hPadding, textY, title.c_str(), true);

      if (!recentBooks[i].author.empty()) {
        auto author = renderer.truncatedText(UI_10_FONT_ID, recentBooks[i].author.c_str(), tileWidth - 2 * hPadding);
        textY += renderer.getLineHeight(UI_10_FONT_ID);
        renderer.drawText(UI_10_FONT_ID, tileX + hPadding, textY, author.c_str(), true);
      }

      if (selected) {
        renderer.drawRect(tileX, tileY, tileWidth, tileHeight, false);
        renderer.drawRect(tileX + 1, tileY + 1, tileWidth - 2, tileHeight - 2, false);
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void ForkDriftTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                    const std::function<std::string(int index)>& buttonLabel,
                                    const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) {
    return;
  }

  const int pad = ForkDriftMetrics::values.contentSidePadding;
  const int availableW = rect.width - 2 * pad;
  const int availableH = rect.height;
  constexpr int iconSize = 24;

  if (buttonCount == 1) {
    const int tileW = availableW;
    const int tileH = std::min(72, availableH);
    const int x = rect.x + pad;
    const int y = rect.y + std::max(0, (availableH - tileH) / 2);
    const bool selected = selectedIndex == 0;

    if (selected) {
      renderer.fillRoundedRect(x, y, tileW, tileH, cornerRadius, Color::LightGray);
    }

    const std::string label = buttonLabel(0);
    const UIIcon icon = rowIcon ? rowIcon(0) : UIIcon::Settings;
    const uint8_t* iconBmp = iconFor(icon, iconSize);
    int textX = x + 12;
    if (iconBmp) {
      renderer.drawIcon(iconBmp, x + 12, y + (tileH - iconSize) / 2, iconSize, iconSize);
      textX += iconSize + hPadding;
    }
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = y + (tileH - lineH) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), true);
    return;
  }

  const int tileW = (availableW - ForkDriftMetrics::values.menuSpacing) / buttonCols;
  const int tileH = (availableH - ForkDriftMetrics::values.menuSpacing) / buttonRows;

  for (int i = 0; i < std::min(buttonCount, buttonCols * buttonRows); i++) {
    const int col = i % buttonCols;
    const int row = i / buttonCols;
    const int x = rect.x + pad + col * (tileW + ForkDriftMetrics::values.menuSpacing);
    const int y = rect.y + row * (tileH + ForkDriftMetrics::values.menuSpacing);
    const bool selected = (selectedIndex == i);

    if (selected) {
      renderer.fillRoundedRect(x, y, tileW, tileH, cornerRadius, Color::LightGray);
    }

    std::string label = buttonLabel(i);
    UIIcon icon = rowIcon ? rowIcon(i) : UIIcon::Folder;
    const uint8_t* iconBmp = iconFor(icon, iconSize);
    int textX = x + 12;
    if (iconBmp) {
      renderer.drawIcon(iconBmp, x + 12, y + (tileH - iconSize) / 2, iconSize, iconSize);
      textX += iconSize + hPadding;
    }
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = y + (tileH - lineH) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, label.c_str(), true);
  }
}
