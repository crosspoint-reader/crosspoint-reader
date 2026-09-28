#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
}  // namespace

int Lyra3CoversTheme::homeCoverThumbHeight(const GfxRenderer& renderer) const {
  const int tileWidth = (renderer.getScreenWidth() - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  // Thumbs cover a (0.6*h, h) target box; in landscape the tile is wider than
  // 0.6 aspect, so request a taller thumb and let the draw crop vertically.
  return std::max(Lyra3CoversMetrics::values.homeCoverHeight, (tileWidth - 2 * hPaddingInSelection) * 5 / 3 + 2);
}

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, homeCoverThumbHeight(renderer));

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              // Fill the fixed tile 1:1 and crop the overflow; the old crop
              // factor went negative for covers narrower than the tile.
              drawCoverThumbFill(renderer, bitmap,
                                 Rect{tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                      tileWidth - 2 * hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight});
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, true);

        if (!hasCover) {
          // Render empty cover
          renderer.fillRect(tileX + hPaddingInSelection,
                            tileY + hPaddingInSelection + (Lyra3CoversMetrics::values.homeCoverHeight / 3),
                            tileWidth - 2 * hPaddingInSelection, 2 * Lyra3CoversMetrics::values.homeCoverHeight / 3,
                            true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      auto titleLines = renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, 3);

      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      const int dynamicTitleBoxHeight = dynamicBlockHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection,
                                 tileWidth, dynamicTitleBoxHeight, cornerRadius, false, false, true, true,
                                 Color::LightGray);
      }

      int currentY = tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
