#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "Utf8.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
}  // namespace

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileHeight = rect.height;
  const int bookTitleHeight = tileHeight - Lyra3CoversMetrics::values.homeCoverHeight - hPaddingInSelection;
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
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, Lyra3CoversMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(Lyra3CoversMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight,
                                  cropX);
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
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      // Wrap title to up to 3 lines (word-wrap by advance width)
      const std::string& lastBookTitle = recentBooks[i].title;
      std::vector<std::string> words;
      words.reserve(8);
      std::string::size_type wordStart = 0;
      std::string::size_type wordEnd = 0;
      // find_first_not_of skips leading/interstitial spaces
      while ((wordStart = lastBookTitle.find_first_not_of(' ', wordEnd)) != std::string::npos) {
        wordEnd = lastBookTitle.find(' ', wordStart);
        if (wordEnd == std::string::npos) wordEnd = lastBookTitle.size();
        words.emplace_back(lastBookTitle.substr(wordStart, wordEnd - wordStart));
      }

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;
      const int spaceWidth = renderer.getSpaceWidth(SMALL_FONT_ID, EpdFontFamily::REGULAR);
      std::vector<std::string> titleLines;
      std::string currentLine;

      for (auto& w : words) {
        if (titleLines.size() >= 3) {
          titleLines.back().append("...");
          while (!titleLines.back().empty() && titleLines.back().size() > 3 &&
                 renderer.getTextWidth(SMALL_FONT_ID, titleLines.back().c_str(), EpdFontFamily::REGULAR) >
                     maxLineWidth) {
            titleLines.back().resize(titleLines.back().size() - 3);
            utf8RemoveLastChar(titleLines.back());
            titleLines.back().append("...");
          }
          break;
        }
        int wordW = renderer.getTextWidth(SMALL_FONT_ID, w.c_str(), EpdFontFamily::REGULAR);
        while (wordW > maxLineWidth && !w.empty()) {
          utf8RemoveLastChar(w);
          std::string withE = w + "...";
          wordW = renderer.getTextWidth(SMALL_FONT_ID, withE.c_str(), EpdFontFamily::REGULAR);
          if (wordW <= maxLineWidth) {
            w = withE;
            break;
          }
        }
        if (w.empty()) continue;  // Skip words that couldn't fit even truncated
        int newW = renderer.getTextAdvanceX(SMALL_FONT_ID, currentLine.c_str(), EpdFontFamily::REGULAR);
        if (newW > 0) newW += spaceWidth;
        newW += renderer.getTextAdvanceX(SMALL_FONT_ID, w.c_str(), EpdFontFamily::REGULAR);
        if (newW > maxLineWidth && !currentLine.empty()) {
          titleLines.push_back(currentLine);
          currentLine = w;
        } else if (currentLine.empty()) {
          currentLine = w;
        } else {
          currentLine.append(" ").append(w);
        }
      }
      if (!currentLine.empty() && titleLines.size() < 3) titleLines.push_back(currentLine);

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
