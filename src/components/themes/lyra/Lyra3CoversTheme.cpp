#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

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
constexpr int coverHeightOffset = 58;
}  // namespace

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const int coverHeight = std::max(120, rect.height - coverHeightOffset);
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      bool anyPending = false;
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        bool tilePending = false;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, coverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              const float bitmapHeight = static_cast<float>(bitmap.getHeight());
              const float bitmapWidth = static_cast<float>(bitmap.getWidth());
              const float ratio = bitmapWidth / bitmapHeight;
              const float tileRatio =
                  static_cast<float>(tileWidth - 2 * hPaddingInSelection) / static_cast<float>(coverHeight);
              const float cropX = std::max(0.0f, 1.0f - (tileRatio / ratio));

              // Clear tile to white before drawing: 1-bit BMPs only draw dark pixels,
              // leaving white pixels transparent — any stale dark content shows through.
              renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                tileWidth - 2 * hPaddingInSelection, coverHeight, false);
              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, coverHeight, cropX);
            } else {
              hasCover = false;
            }
            file.close();
          } else {
            hasCover = false;
            tilePending = true;  // path exists but BMP not ready yet
            anyPending = true;
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          coverHeight, true);

        if (!hasCover) {
          if (tilePending) {
            // Cover is being generated — show a loading label centred in the tile
            const char* loadingText = tr(STR_LOADING);
            const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
            const int textH = renderer.getLineHeight(SMALL_FONT_ID);
            renderer.drawText(SMALL_FONT_ID,
                              tileX + hPaddingInSelection + (tileWidth - 2 * hPaddingInSelection - textW) / 2,
                              tileY + hPaddingInSelection + (coverHeight - textH) / 2, loadingText, true);
          } else {
            // No cover at all — render empty cover placeholder
            renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (coverHeight / 3),
                              tileWidth - 2 * hPaddingInSelection, 2 * coverHeight / 3, true);
            renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
          }
        }
      }

      // Only cache the frame buffer once all tiles are definitively resolved.
      // If any cover is still being generated we keep coverRendered=false so the next render will retry.
      if (!anyPending) {
        coverBufferStored = storeCoverBuffer();
        coverRendered = coverBufferStored;
      }
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);
      const int progressPercent = getRecentBookProgressPercent(recentBooks[i]);

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
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, dynamicTitleBoxHeight,
                                 cornerRadius, false, false, true, true, Color::LightGray);
      }

      drawProgressBadge(static_cast<const GfxRenderer&>(renderer),
                        Rect{tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                             tileWidth - 2 * hPaddingInSelection, coverHeight},
                        progressPercent);

      int currentY = tileY + coverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
