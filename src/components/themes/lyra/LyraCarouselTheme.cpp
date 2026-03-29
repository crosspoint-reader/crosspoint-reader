#include "LyraCarouselTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "components/icons/book.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"

namespace {
// Cover layout — centre cover dominates, sides slide kOverlap px behind it
constexpr int kCenterCoverMaxW = 340;
constexpr int kCenterCoverMaxH = LyraCarouselMetrics::values.homeCoverHeight - 50;  // 550; frees 50px for title+dots
constexpr int kSideCoverMaxW = 200;
constexpr int kSideCoverMaxH = LyraCarouselMetrics::values.homeCoverHeight - 210;  // 390
constexpr int kOverlap = 60;
constexpr int kCoverTopPad = 10;

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kDotSize = 8;    // px square dot
constexpr int kDotGap = 6;     // px between dots

constexpr int kCornerRadius = 6;
constexpr int kSelectionLineW = 2;
constexpr int kCenterOutlineW = 4;  // white ring around centre cover; black border overlays when selected

// Icon row — icons are 32×32 bitmaps; drawIcon does NOT scale
constexpr int kMenuIconSize = 32;  // must match actual bitmap dimensions
constexpr int kMenuIconPad = 14;   // symmetric vertical padding → tile height = 60
// Row is anchored to the bottom of the screen, just above button hints
constexpr int kButtonHintsH = LyraCarouselMetrics::values.buttonHintsHeight;

int lastCarouselSelectorIndex = -1;

const uint8_t* iconBitmapFor(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Book:
      return BookIcon;
    default:
      return nullptr;
  }
}
}  // namespace

// ---------------------------------------------------------------------------
// Carousel cover strip
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                            bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                            std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  // When navigating the icon row, keep showing the last carousel position —
  // falling back to 0 on first use (lastCarouselSelectorIndex == -1).
  const bool inCarouselRow = (selectorIndex < bookCount);
  const int centerIdx =
      inCarouselRow ? selectorIndex : (lastCarouselSelectorIndex >= 0 ? lastCarouselSelectorIndex : 0);

  if (centerIdx != lastCarouselSelectorIndex) {
    coverRendered = false;
    coverBufferStored = false;
  }

  const int screenW = renderer.getScreenWidth();
  const int centerTileY = rect.y + kCoverTopPad;
  const int sideTileY = centerTileY + (kCenterCoverMaxH - kSideCoverMaxH) / 2;

  const int centerX = (screenW - kCenterCoverMaxW) / 2;
  const int leftX = centerX - kSideCoverMaxW + kOverlap;
  const int rightX = centerX + kCenterCoverMaxW - kOverlap;

  auto drawCover = [&](int bookIdx, int x, int y, int maxW, int maxH) {
    if (bookIdx < 0 || bookIdx >= bookCount) return;
    const RecentBook& book = recentBooks[bookIdx];
    bool hasCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath =
          UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselMetrics::values.homeCoverHeight);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Height always fills the tile. Only crop horizontally if the cover is
          // wider than the tile; narrow covers get white space on the sides.
          const float bmpRatio =
              static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float tileRatio = static_cast<float>(maxW) / static_cast<float>(maxH);
          const float cropX = (bmpRatio > tileRatio) ? (1.0f - tileRatio / bmpRatio) : 0.0f;
          renderer.drawBitmap(bitmap, x, y, maxW, maxH, cropX, 0.0f);
          hasCover = true;
        }
        file.close();
      }
    }
    if (!hasCover) {
      renderer.drawRect(x, y, maxW, maxH, true);
      renderer.fillRect(x, y + maxH / 3, maxW, 2 * maxH / 3, true);
      renderer.drawIcon(CoverIcon, x + maxW / 2 - 16, y + 8, 32, 32);
    }
  };

  if (!coverRendered) {
    lastCarouselSelectorIndex = centerIdx;

    // Clear the entire cover tile to white so stale pixels from old positions
    // don't persist (drawBitmap only sets black pixels, never clears).
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    // Sides first so centre renders on top
    if (bookCount >= 2) {
      const int prevIdx = (centerIdx + bookCount - 1) % bookCount;
      const int nextIdx = (centerIdx + 1) % bookCount;
      drawCover(prevIdx, leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH);
      renderer.drawRect(leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, true);
      drawCover(nextIdx, rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH);
      renderer.drawRect(rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, true);
    }
    // Clear a white outline ring around the centre cover, then draw the cover
    // inside it. The white ring always separates the centre from the sides.
    renderer.fillRect(centerX - kCenterOutlineW, centerTileY - kCenterOutlineW,
                      kCenterCoverMaxW + 2 * kCenterOutlineW, kCenterCoverMaxH + 2 * kCenterOutlineW, false);
    drawCover(centerIdx, centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH);

    // Title below centre cover
    const int titleY = centerTileY + kCenterCoverMaxH + 8;
    const std::string titleTrunc =
        renderer.truncatedText(kTitleFontId, recentBooks[centerIdx].title.c_str(), kCenterCoverMaxW);
    const int titleW = renderer.getTextWidth(kTitleFontId, titleTrunc.c_str());
    renderer.drawText(kTitleFontId, centerX + (kCenterCoverMaxW - titleW) / 2, titleY, titleTrunc.c_str(), true);

    // Dot indicator — filled square for current book, outlined for others
    const int dotsY = titleY + renderer.getLineHeight(kTitleFontId) + 4;
    const int totalDotsW = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = centerX + (kCenterCoverMaxW - totalDotsW) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIdx) {
        renderer.fillRect(dotX, dotsY, kDotSize, kDotSize, true);
      } else {
        renderer.drawRect(dotX, dotsY, kDotSize, kDotSize, true);
      }
      dotX += kDotSize + kDotGap;
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Selection border on top of centre cover when carousel row is active
  if (selectorIndex < bookCount) {
    renderer.drawRoundedRect(centerX - kCenterOutlineW, centerTileY - kCenterOutlineW,
                             kCenterCoverMaxW + 2 * kCenterOutlineW, kCenterCoverMaxH + 2 * kCenterOutlineW,
                             kSelectionLineW, kCornerRadius + 2, true);
  }
}

// ---------------------------------------------------------------------------
// Horizontal icon-only menu row — anchored to bottom of screen
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                       const std::function<std::string(int index)>& buttonLabel,
                                       const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;

  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  const int tileW = renderer.getScreenWidth() / buttonCount;
  // Anchor row just above button hints, ignoring rect.y which may be off-screen
  // for large cover tiles
  const int rowY = renderer.getScreenHeight() - kButtonHintsH - tileH;

  for (int i = 0; i < buttonCount; ++i) {
    const int tileX = i * tileW;

    if (selectedIndex == i) {
      renderer.fillRoundedRect(tileX + 4, rowY + 4, tileW - 8, tileH - 8, kCornerRadius, Color::LightGray);
    }

    if (rowIcon != nullptr) {
      const uint8_t* bmp = iconBitmapFor(rowIcon(i));
      if (bmp != nullptr) {
        const int iconX = tileX + (tileW - kMenuIconSize) / 2;
        const int iconY = rowY + kMenuIconPad;
        renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
      }
    }
  }
}
