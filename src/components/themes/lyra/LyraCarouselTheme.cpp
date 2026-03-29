#include "LyraCarouselTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"

namespace {
// Cover layout — centre cover dominates, sides slide kOverlap px behind it
constexpr int kCenterCoverMaxW = 340;
constexpr int kCenterCoverMaxH = LyraCarouselMetrics::values.homeCoverHeight;
constexpr int kSideCoverMaxW = 280;
constexpr int kSideCoverMaxH = LyraCarouselMetrics::values.homeCoverHeight - 40;
constexpr int kOverlap = 60;   // px each side cover hides behind centre
constexpr int kCoverTopPad = 10;

constexpr int kCornerRadius = 6;
constexpr int kSelectionLineW = 2;

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
          // Crop-to-fill: compute cropX or cropY so the image covers the entire
          // tile with no white bars, cropping the excess symmetrically.
          const float bmpW = static_cast<float>(bitmap.getWidth());
          const float bmpH = static_cast<float>(bitmap.getHeight());
          const float tileRatio = static_cast<float>(maxW) / static_cast<float>(maxH);
          const float bmpRatio = bmpW / bmpH;
          float cropX = 0.0f;
          float cropY = 0.0f;
          if (bmpRatio > tileRatio) {
            cropX = 1.0f - tileRatio / bmpRatio;  // bitmap too wide: crop sides
          } else {
            cropY = 1.0f - bmpRatio / tileRatio;  // bitmap too tall: crop top/bottom
          }
          renderer.drawBitmap(bitmap, x, y, maxW, maxH, cropX, cropY);
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
      drawCover(nextIdx, rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH);
    }
    // Clear centre area to white before drawing centre cover so the centre
    // cover always occludes any black pixels painted by the side covers.
    renderer.fillRect(centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH, false);
    drawCover(centerIdx, centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH);

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Selection border on top of centre cover when carousel row is active
  if (selectorIndex < bookCount) {
    renderer.drawRoundedRect(centerX - 4, centerTileY - 4, kCenterCoverMaxW + 8, kCenterCoverMaxH + 8,
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
