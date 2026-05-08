#include "LyraCarouselTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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
#include "components/icons/weather32.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {
// Cover layout — centre cover dominates, sides slide kOverlap px behind it
constexpr int kCenterCoverMaxW = LyraCarouselTheme::kCenterCoverW;
constexpr int kCenterCoverMaxH = LyraCarouselTheme::kCenterCoverH;
constexpr int kSideCoverMaxW = LyraCarouselTheme::kSideCoverW;
constexpr int kSideCoverMaxH = LyraCarouselTheme::kSideCoverH;
constexpr int kOverlap = 60;
constexpr int kCoverTopPad = 10;

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kDotSize = 8;  // px square dot
constexpr int kDotGap = 6;   // px between dots

constexpr int kCornerRadius = 6;
constexpr int kThinOutlineW = 1;    // always-visible outline around centre cover
constexpr int kSelectionLineW = 3;  // thicker outline when centre cover is selected
constexpr int kCenterOutlineW = 4;  // white ring around centre cover

// Icon row — icons are 32×32 bitmaps; drawIcon does NOT scale
constexpr int kMenuIconSize = 32;  // must match actual bitmap dimensions
constexpr int kMenuIconPad = 14;   // symmetric vertical padding → tile height = 60
constexpr int kHighlightPad = 12;  // horizontal padding around the icon on each side
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
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Weather:
      return Weather32Icon;
    case UIIcon::Wifi:
      return WifiIcon;
    case UIIcon::Hotspot:
      return HotspotIcon;
    default:
      return nullptr;
  }
}
// ---------------------------------------------------------------------------
// Static frame cache — survives HomeActivity re-creation so that returning to
// home after settings doesn't re-read covers from SD.
// Freed explicitly via invalidateFrameCache() before entering the reader.
// ---------------------------------------------------------------------------
constexpr int kFrameCount = 1;
uint8_t* gCachedFrames[kFrameCount] = {};
int gCachedFrameBookIdx[kFrameCount] = {-1};
int gCachedFrameCount = 0;
std::string gCacheKey;

int findFrameSlot(int bookIdx) {
  for (int i = 0; i < kFrameCount; ++i) {
    if (gCachedFrameBookIdx[i] == bookIdx && gCachedFrames[i] != nullptr) return i;
  }
  return -1;
}

void freeFrameCache() {
  for (int i = 0; i < kFrameCount; ++i) {
    if (gCachedFrames[i]) {
      free(gCachedFrames[i]);
      gCachedFrames[i] = nullptr;
    }
    gCachedFrameBookIdx[i] = -1;
  }
  gCachedFrameCount = 0;
  gCacheKey.clear();
}
}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
void LyraCarouselTheme::setPreRenderIndex(int idx) { lastCarouselSelectorIndex = idx; }

void LyraCarouselTheme::invalidateFrameCache() { freeFrameCache(); }

void LyraCarouselTheme::onBookWillClose(const std::string& /*path*/, Epub* epub, Xtc* xtc, Txt* /*txt*/) {
  if (epub) {
    epub->generateThumbBmp(kCenterCoverW, kCenterCoverH);
    epub->generateThumbBmp(kSideCoverW, kSideCoverH);
  }
  if (xtc) {
    xtc->generateThumbBmp(kCenterCoverW, kCenterCoverH);
    xtc->generateThumbBmp(kSideCoverW, kSideCoverH);
  }
  // txt files have no cover image — nothing to generate
  invalidateFrameCache();
}

// ---------------------------------------------------------------------------
// tryFastHomeRender — pre-renders carousel frames and composites them
// ---------------------------------------------------------------------------
namespace {
void renderOneCarouselFrame(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int bookIdx, int slotIdx,
                            const ThemeMetrics& metrics) {
  if (!gCachedFrames[slotIdx]) return;
  const int pageWidth = renderer.getScreenWidth();
  const int bookCount = static_cast<int>(recentBooks.size());
  bool d1 = false, d2 = false, d3 = false;

  lastCarouselSelectorIndex = bookIdx;
  renderer.clearScreen();
  UITheme::getInstance().getTheme().drawRecentBookCover(
      renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight}, recentBooks, bookCount, d1, d2,
      d3, []() { return true; });

  memcpy(gCachedFrames[slotIdx], renderer.getFrameBuffer(), renderer.getBufferSize());
  gCachedFrameBookIdx[slotIdx] = bookIdx;
}

void updateSlidingWindow(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int centerIdx,
                         const ThemeMetrics& metrics) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount <= kFrameCount || gCachedFrameCount == 0) return;

  const int prevIdx = (centerIdx + bookCount - 1) % bookCount;
  const int nextIdx = (centerIdx + 1) % bookCount;
  const bool hasPrev = findFrameSlot(prevIdx) >= 0;
  const bool hasNext = findFrameSlot(nextIdx) >= 0;
  if (hasPrev && hasNext) return;

  const int missingIdx = !hasPrev ? prevIdx : nextIdx;
  int evictSlot = -1, maxDist = -1;
  for (int i = 0; i < kFrameCount; ++i) {
    if (!gCachedFrames[i]) continue;
    const int b = gCachedFrameBookIdx[i];
    if (b == centerIdx || (hasPrev && b == prevIdx) || (hasNext && b == nextIdx)) continue;
    const int diff = std::abs(b - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }
  if (evictSlot >= 0) renderOneCarouselFrame(renderer, recentBooks, missingIdx, evictSlot, metrics);
}
}  // namespace

bool LyraCarouselTheme::tryFastHomeRender(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks,
                                          int selectorIndex, int menuCount,
                                          const std::function<std::string(int)>& menuLabel,
                                          const std::function<UIIcon(int)>& menuIcon, const char* hintBtn1,
                                          const char* hintBtn2, const char* hintBtn3, const char* hintBtn4) const {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const size_t bufferSize = renderer.getBufferSize();
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  // Build cache key from book paths
  std::string newKey;
  newKey.reserve(128);
  for (const auto& b : recentBooks) {
    newKey += b.path;
    newKey += '\0';
  }

  if (newKey != gCacheKey || gCachedFrameCount == 0) {
    // Free old cache and allocate fresh frames
    freeFrameCache();
    const int frameCount = std::min(bookCount, kFrameCount);
    for (int i = 0; i < frameCount; ++i) {
      gCachedFrames[i] = static_cast<uint8_t*>(malloc(bufferSize));
      if (!gCachedFrames[i]) {
        LOG_ERR("CAROUSEL", "tryFastHomeRender: malloc failed for frame %d", i);
        freeFrameCache();
        return false;
      }
    }
    // Render only the initially-selected frame; neighbours are filled lazily.
    const int initialIdx = (selectorIndex < bookCount) ? selectorIndex : 0;
    renderOneCarouselFrame(renderer, recentBooks, initialIdx, 0, metrics);
    gCachedFrameCount = frameCount;
    gCacheKey = newKey;
  }

  const bool inCarouselRow = (selectorIndex < bookCount);
  const int centerIdx =
      inCarouselRow ? selectorIndex : (lastCarouselSelectorIndex >= 0 ? lastCarouselSelectorIndex : 0);
  int slotIdx = findFrameSlot(centerIdx);
  if (slotIdx < 0) {
    slotIdx = 0;
  }
  if (!gCachedFrames[slotIdx]) {
    gCachedFrames[slotIdx] = static_cast<uint8_t*>(malloc(bufferSize));
    if (!gCachedFrames[slotIdx]) {
      LOG_ERR("CAROUSEL", "tryFastHomeRender: malloc failed for frame %d", slotIdx);
      return false;
    }
  }
  if (findFrameSlot(centerIdx) < 0 || gCachedFrameBookIdx[slotIdx] != centerIdx) {
    renderOneCarouselFrame(renderer, recentBooks, centerIdx, slotIdx, metrics);
  }

  memcpy(frameBuffer, gCachedFrames[slotIdx], bufferSize);
  UITheme::getInstance().getTheme().drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                                               nullptr);

  // Overlay the selection border when carousel row is active
  if (inCarouselRow) {
    const int screenW = renderer.getScreenWidth();
    const int centerTileY = metrics.homeTopPadding + kCoverTopPad;
    const int centerX = (screenW - kCenterCoverMaxW) / 2;
    renderer.drawRoundedRect(centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH, kSelectionLineW, kCornerRadius,
                             true);
  }

  // Menu row
  const int menuIdx = inCarouselRow ? -1 : (selectorIndex - bookCount);
  UITheme::getInstance().getTheme().drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      menuCount, menuIdx, menuLabel, menuIcon);

  // Button hints
  UITheme::getInstance().getTheme().drawButtonHints(renderer, hintBtn1, hintBtn2, hintBtn3, hintBtn4);

  renderer.displayBuffer();
  updateSlidingWindow(renderer, recentBooks, centerIdx, metrics);
  return true;
}

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
  int centerIdx = inCarouselRow ? selectorIndex : (lastCarouselSelectorIndex >= 0 ? lastCarouselSelectorIndex : 0);

  if (centerIdx >= bookCount) {
    centerIdx = bookCount - 1;
    coverRendered = false;
    coverBufferStored = false;
  }

  // cppcheck-suppress knownConditionTrueFalse
  // Reachable as false when navigating the icon row with a previously-set
  // lastCarouselSelectorIndex; cppcheck only models the inCarouselRow=true path.
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

  // Returns true if a book exists at bookIdx (cover image or placeholder drawn).
  // Returns false only when the slot has no book — caller skips the border too.
  auto drawCover = [&](int bookIdx, int x, int y, int maxW, int maxH) -> bool {
    if (bookIdx < 0 || bookIdx >= bookCount) return false;
    const RecentBook& book = recentBooks[bookIdx];
    // Side tiles may extend off-screen — only round corners that are on-screen.
    const bool roundLeft = (x >= 0);
    const bool roundRight = (x + maxW <= screenW);
    bool hasCover = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, maxW, maxH);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Height always fills the tile. Only crop horizontally if the cover is
          // wider than the tile; narrow covers get white space on the sides.
          const float bmpRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float tileRatio = static_cast<float>(maxW) / static_cast<float>(maxH);
          const float cropX = (bmpRatio > tileRatio) ? (1.0f - tileRatio / bmpRatio) : 0.0f;
          renderer.drawBitmap(bitmap, x, y, maxW, maxH, cropX, 0.0f);
          // Clear only the pixels outside the arc in each corner.
          // The arc centre for the top-left corner is (x+r, y+r). A pixel at
          // (x+dx, y+dy) is outside the arc when its distance from that centre
          // exceeds r, i.e. (r-1-dx)²+(r-1-dy)² > (r-1)².
          for (int dy = 0; dy < kCornerRadius; ++dy) {
            for (int dx = 0; dx < kCornerRadius; ++dx) {
              const int ex = kCornerRadius - 1 - dx;
              const int ey = kCornerRadius - 1 - dy;
              if (ex * ex + ey * ey > (kCornerRadius - 1) * (kCornerRadius - 1)) {
                if (roundLeft) {
                  renderer.drawPixel(x + dx, y + dy, false);             // top-left
                  renderer.drawPixel(x + dx, y + maxH - 1 - dy, false);  // bottom-left
                }
                if (roundRight) {
                  renderer.drawPixel(x + maxW - 1 - dx, y + dy, false);             // top-right
                  renderer.drawPixel(x + maxW - 1 - dx, y + maxH - 1 - dy, false);  // bottom-right
                }
              }
            }
          }
          renderer.drawRoundedRect(x, y, maxW, maxH, kThinOutlineW, kCornerRadius, roundLeft, roundRight, roundLeft,
                                   roundRight, true);
          hasCover = true;
        }
        file.close();
      }
    }
    if (!hasCover) {
      renderer.drawRoundedRect(x, y, maxW, maxH, 1, kCornerRadius, roundLeft, roundRight, roundLeft, roundRight, true);
      renderer.fillRoundedRect(x, y + maxH / 3, maxW, 2 * maxH / 3, kCornerRadius, /*roundTopLeft=*/false,
                               /*roundTopRight=*/false, /*roundBottomLeft=*/roundLeft, /*roundBottomRight=*/roundRight,
                               Color::Black);
      renderer.drawIcon(CoverIcon, x + maxW / 2 - 16, y + 8, 32, 32);
    }
    return true;
  };

  if (!coverRendered) {
    lastCarouselSelectorIndex = centerIdx;

    // Clear from the top of the tile down through the author/title text area.
    // Use absolute coordinates so the clear covers the text regardless of what
    // rect.height HomeActivity computed (it may be smaller than homeCoverTileHeight).
    const int textAreaBottom = centerTileY + kCenterCoverMaxH              // bottom of centre cover
                               + 8 + kDotSize                              // dots
                               + 6 + renderer.getLineHeight(kTitleFontId)  // author line
                               + 2 + renderer.getLineHeight(kTitleFontId)  // title line
                               + 4;                                        // small margin
    renderer.fillRect(rect.x, rect.y, rect.width, textAreaBottom - rect.y, false);

    // Sides first so centre renders on top.
    // Left side only when there are 3+ books; right side when there are 2+ books.
    // Border only drawn if a cover image was actually rendered (no placeholders).
    const int prevIdx = (centerIdx + bookCount - 1) % bookCount;
    const int nextIdx = (centerIdx + 1) % bookCount;
    if (bookCount >= 3) {
      if (drawCover(prevIdx, leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH)) {
        const bool rl = (leftX >= 0), rr = (leftX + kSideCoverMaxW <= screenW);
        renderer.drawRoundedRect(leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, 1, kCornerRadius, rl, rr, rl, rr,
                                 true);
      }
    }
    if (bookCount >= 2) {
      if (drawCover(nextIdx, rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH)) {
        const bool rl = (rightX >= 0), rr = (rightX + kSideCoverMaxW <= screenW);
        renderer.drawRoundedRect(rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, 1, kCornerRadius, rl, rr, rl, rr,
                                 true);
      }
    }

    // Clear a white outline ring around the centre cover, then draw the cover
    // inside it. The white ring always separates the centre from the sides.
    renderer.fillRect(centerX - kCenterOutlineW, centerTileY - kCenterOutlineW, kCenterCoverMaxW + 2 * kCenterOutlineW,
                      kCenterCoverMaxH + 2 * kCenterOutlineW, false);
    drawCover(centerIdx, centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH);

    // Dots — centred over the cover tile, count = actual book count
    const int dotsY = centerTileY + kCenterCoverMaxH + 8;
    const int totalDotsW = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = centerX + (kCenterCoverMaxW - totalDotsW) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIdx)
        renderer.fillRect(dotX, dotsY, kDotSize, kDotSize, true);
      else
        renderer.drawRect(dotX, dotsY, kDotSize, kDotSize, true);
      dotX += kDotSize + kDotGap;
    }

    // Author then title below dots
    const int authorY = dotsY + kDotSize + 6;
    const std::string authorTrunc =
        renderer.truncatedText(kTitleFontId, recentBooks[centerIdx].author.c_str(), kCenterCoverMaxW);
    const int authorW = renderer.getTextWidth(kTitleFontId, authorTrunc.c_str());
    renderer.drawText(kTitleFontId, centerX + (kCenterCoverMaxW - authorW) / 2, authorY, authorTrunc.c_str(), true);

    const int titleY = authorY + renderer.getLineHeight(kTitleFontId) + 2;
    const std::string titleTrunc =
        renderer.truncatedText(kTitleFontId, recentBooks[centerIdx].title.c_str(), kCenterCoverMaxW);
    const int titleW = renderer.getTextWidth(kTitleFontId, titleTrunc.c_str());
    renderer.drawText(kTitleFontId, centerX + (kCenterCoverMaxW - titleW) / 2, titleY, titleTrunc.c_str(), true);

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Always outline the centre cover at its own edge (white ring sits outside the black line);
  // thicker when the carousel row is active
  const int outlineW = inCarouselRow ? kSelectionLineW : kThinOutlineW;
  renderer.drawRoundedRect(centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH, outlineW, kCornerRadius, true);
}

// ---------------------------------------------------------------------------
// Horizontal icon-only menu row — anchored to bottom of screen
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                       const std::function<std::string(int index)>& buttonLabel,
                                       const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;

  const int screenW = renderer.getScreenWidth();
  const int tileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
  // Anchor row just above button hints, ignoring rect.y which may be off-screen for large cover tiles
  const int rowY = renderer.getScreenHeight() - kButtonHintsH - tileH;

  // How many icons fit side-by-side? Each needs at least (kMenuIconSize + 2*kHighlightPad).
  const int minTileW = kMenuIconSize + 2 * kHighlightPad;
  const int visibleCount = std::min(buttonCount, screenW / minTileW);
  const int tileW = screenW / visibleCount;

  // Sliding window: keep selectedIndex centred when we can't show all icons.
  int firstVisible = 0;
  if (selectedIndex >= 0 && visibleCount < buttonCount) {
    firstVisible = selectedIndex - visibleCount / 2;
    firstVisible = std::max(0, std::min(firstVisible, buttonCount - visibleCount));
  }

  // Draw the selected item's label in the header area so the user knows what they're about to open.
  if (selectedIndex >= 0 && buttonLabel != nullptr) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const std::string label = buttonLabel(selectedIndex);
    const int labelY = metrics.topPadding + 5;  // same y as battery / clock
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    const int labelX = (screenW - labelW) / 2;
    renderer.drawText(UI_12_FONT_ID, labelX, labelY, label.c_str(), true, EpdFontFamily::BOLD);
  }

  for (int slot = 0; slot < visibleCount; ++slot) {
    const int i = firstVisible + slot;
    if (i >= buttonCount) break;

    const int tileX = slot * tileW;
    const int iconX = tileX + (tileW - kMenuIconSize) / 2;
    const int iconY = rowY + kMenuIconPad;

    const bool selected = (selectedIndex == i);
    if (selected) {
      const int highlightSize = kMenuIconSize + 2 * kHighlightPad;
      const int highlightY = rowY + (tileH - highlightSize) / 2;
      renderer.fillRoundedRect(iconX - kHighlightPad, highlightY, highlightSize, highlightSize, kCornerRadius,
                               Color::Black);
    }

    if (rowIcon != nullptr) {
      const uint8_t* bmp = iconBitmapFor(rowIcon(i));
      if (bmp != nullptr) {
        if (selected)
          renderer.drawIconInverted(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
        else
          renderer.drawIcon(bmp, iconX, iconY, kMenuIconSize, kMenuIconSize);
      }
    }

    // Overflow indicators: small filled square at left/right edge when icons are clipped
    if (visibleCount < buttonCount) {
      constexpr int kArrowDotR = 3;
      const int arrowY = rowY + tileH / 2;
      if (firstVisible > 0 && slot == 0)
        renderer.fillRect(2, arrowY - kArrowDotR, kArrowDotR * 2, kArrowDotR * 2, true);
      if (firstVisible + visibleCount < buttonCount && slot == visibleCount - 1)
        renderer.fillRect(screenW - 2 - kArrowDotR * 2, arrowY - kArrowDotR, kArrowDotR * 2, kArrowDotR * 2, true);
    }
  }
}

// ---------------------------------------------------------------------------
// List — solid black highlight, inverted text and icons on selected row
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                 const std::function<std::string(int index)>& rowTitle,
                                 const std::function<std::string(int index)>& rowSubtitle,
                                 const std::function<UIIcon(int index)>& rowIcon,
                                 const std::function<std::string(int index)>& rowValue, bool highlightValue) const {
  constexpr int hPad = 8;
  constexpr int listIconSz = 24;
  constexpr int mainMenuIconSz = 32;
  constexpr int maxValWidth = 200;
  constexpr int cornerRadius = 6;

  const int rowHeight = (rowSubtitle != nullptr) ? LyraCarouselMetrics::values.listWithSubtitleRowHeight
                                                 : LyraCarouselMetrics::values.listRowHeight;
  const int pageItems = rect.height / rowHeight;
  if (pageItems <= 0 || itemCount <= 0) return;
  const int totalPages = (itemCount + pageItems - 1) / pageItems;

  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraCarouselMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraCarouselMetrics::values.scrollBarWidth, scrollBarY,
                      LyraCarouselMetrics::values.scrollBarWidth, scrollBarHeight, true);
  }

  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraCarouselMetrics::values.scrollBarWidth + LyraCarouselMetrics::values.scrollBarRightOffset)
                      : 1);

  // Solid black highlight bar — skip if selected item is a separator
  const bool selectedIsSeparator = (selectedIndex >= 0 && selectedIndex < itemCount && rowTitle != nullptr &&
                                    UITheme::isSeparatorTitle(rowTitle(selectedIndex)));
  if (selectedIndex >= 0 && !selectedIsSeparator) {
    renderer.fillRoundedRect(
        rect.x + LyraCarouselMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
        contentWidth - LyraCarouselMetrics::values.contentSidePadding * 2, rowHeight, kCornerRadius, Color::Black);
  }

  int textX = rect.x + LyraCarouselMetrics::values.contentSidePadding + hPad;
  int textWidth = contentWidth - LyraCarouselMetrics::values.contentSidePadding * 2 - hPad * 2;
  int iconSize = 0;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSz : listIconSz;
    textX += iconSize + hPad;
    textWidth -= iconSize + hPad;
  }

  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  const int iconY = (rowSubtitle != nullptr) ? 16 : 10;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    const bool sel = (i == selectedIndex);
    int rowTextWidth = textWidth;

    int valueWidth = 0;
    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPad;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    if (UITheme::isSeparatorTitle(itemName)) {
      itemName = UITheme::stripSeparatorTitle(itemName);
      drawListSeparator(renderer,
                        Rect{rect.x + LyraCarouselMetrics::values.contentSidePadding, itemY,
                             contentWidth - LyraCarouselMetrics::values.contentSidePadding * 2, rowHeight},
                        textX, rowTextWidth, itemName);
      continue;
    }
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), !sel);

    if (rowIcon != nullptr) {
      const uint8_t* iconBitmap = iconForName(rowIcon(i), iconSize);
      if (iconBitmap != nullptr) {
        const int ix = rect.x + LyraCarouselMetrics::values.contentSidePadding + hPad;
        if (sel)
          renderer.drawIconInverted(iconBitmap, ix, itemY + iconY, iconSize, iconSize);
        else
          renderer.drawIcon(iconBitmap, ix, itemY + iconY, iconSize, iconSize);
      }
    }

    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), !sel);
    }

    if (!valueText.empty()) {
      if (sel && highlightValue) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - LyraCarouselMetrics::values.contentSidePadding - hPad - valueWidth, itemY,
            valueWidth + hPad, rowHeight, cornerRadius, Color::Black);
      }
      renderer.drawText(UI_10_FONT_ID,
                        rect.x + contentWidth - LyraCarouselMetrics::values.contentSidePadding - valueWidth, itemY + 6,
                        valueText.c_str(), !sel);
    }
  }
}

// ---------------------------------------------------------------------------
// Tab bar — solid black background + solid black active tab, inverted text
// ---------------------------------------------------------------------------
void LyraCarouselTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                   bool selected) const {
  constexpr int hPad = 8;
  int currentX = rect.x + LyraCarouselMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPad, rect.height - 4, kCornerRadius,
                                 Color::Black);
      } else {
        renderer.drawRoundedRect(currentX, rect.y, textWidth + 2 * hPad, rect.height - 3, 1, kCornerRadius, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPad, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraCarouselMetrics::values.tabSpacing + 2 * hPad;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}
