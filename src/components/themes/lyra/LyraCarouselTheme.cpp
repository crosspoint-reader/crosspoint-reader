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
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "SerialTestApi.h"
#include "UiFontScale.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/ellipsis.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/weather32.h"
#include "components/themes/ListTouchBand.h"
#include "components/themes/TapTargets.h"
#include "components/themes/lyra/CarouselCoverLayout.h"
#include "fontIds.h"

namespace {
// Cover layout — centre cover dominates, sides slide kOverlap px behind it
constexpr int kCenterCoverMaxW = LyraCarouselTheme::kCenterCoverW;
constexpr int kCenterCoverMaxH = LyraCarouselTheme::kCenterCoverH;
constexpr int kSideCoverMaxW = LyraCarouselTheme::kSideCoverW;
constexpr int kSideCoverMaxH = LyraCarouselTheme::kSideCoverH;
constexpr int kOverlap = 60;
constexpr int kCoverTopPad = 10;

// The one statement of the carousel's tile geometry. Both the draw and the tap targets read it
// through CarouselCoverLayout::compute(), so a slot cannot be painted at one place and made
// tappable at another -- which is exactly how the left neighbour came to be drawn but dead.
constexpr CarouselCoverLayout::Dimensions kCoverDims{kCenterCoverMaxW, kCenterCoverMaxH, kSideCoverMaxW,
                                                     kSideCoverMaxH,   kOverlap,         kCoverTopPad};

// Publish the three carousel slots for touch. Factored out because the covers are painted on
// two different paths -- the full drawRecentBookCover() and tryFastHomeRender(), which restores
// them from a cached region and never calls it -- and a tap has to work on both.
//
// Centre is recorded FIRST because the side covers slide kOverlap px BEHIND it: the draw order
// puts the centre on top, and TapTargets::hitTestIn returns the first rect containing the point,
// so recording the centre first makes the overlap resolve to what the reader can actually see.
void recordCarouselCoverTargets(const int screenW, const int screenH, const int rectY, const int centerIdx,
                                const int bookCount) {
  const CarouselCoverLayout::Slots slots =
      CarouselCoverLayout::compute(kCoverDims, screenW, rectY, centerIdx, bookCount);
  TapTargets::Recorder::Builder targets;
  for (const CarouselCoverLayout::Slot* slot : {&slots.centre, &slots.left, &slots.right}) {
    if (slot->bookIndex < 0) continue;
    // Clipped to the screen before recording: the left tile's x is centreX - sideW + overlap,
    // which is NEGATIVE on a narrow panel (-40 at 540 px), so the unclipped rect claimed the
    // empty margin beside a cover that is only partly drawn. A press there opened a book the
    // reader could not see under their finger.
    const CarouselCoverLayout::Slot visible = CarouselCoverLayout::clampToScreen(*slot, screenW, screenH);
    if (visible.w <= 0 || visible.h <= 0) continue;
    targets.add(visible.x, visible.y, visible.w, visible.h, visible.bookIndex);
  }
  TapTargets::homeCovers().record(targets);
}

constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kDotSize = 8;  // px square dot
// Breathing room for the history line, which is measured against the screen rather than the cover.
constexpr int kHistorySideMargin = 10;
constexpr int kDotGap = 6;  // px between dots

constexpr int kCornerRadius = 6;
constexpr int kThinOutlineW = 1;    // always-visible outline around centre cover
constexpr int kSelectionLineW = 3;  // thicker outline when centre cover is selected
constexpr int kCenterOutlineW = 4;  // white ring around centre cover

// Icon row — icons are 32×32 bitmaps; drawIcon does NOT scale
constexpr int kMenuIconSize = 32;  // must match actual bitmap dimensions
constexpr int kMenuIconPad = 14;   // symmetric vertical padding → tile height = 60
constexpr int kHighlightPad = 12;  // horizontal padding around the icon on each side
// Row is anchored to the bottom of the screen, just above button hints. Read live rather than
// from the constexpr table: the strip grows with the UI font size.
int buttonHintsHeight() { return UITheme::getInstance().getMetrics().buttonHintsHeight; }
constexpr int kMenuTileH = kMenuIconPad + kMenuIconSize + kMenuIconPad;
constexpr int kMenuHighlightSize = kMenuIconSize + 2 * kHighlightPad;
int menuRowY(const GfxRenderer& renderer) { return renderer.getScreenHeight() - buttonHintsHeight() - kMenuTileH; }
// The highest pixel the icon row ever paints: the selected icon's highlight square, which stands
// taller than the icons themselves. Text above the row has to end here, not at the icons, or a
// selected icon covers it.
int menuInkTop(const GfxRenderer& renderer) { return menuRowY(renderer) + (kMenuTileH - kMenuHighlightSize) / 2; }

// Vertical gaps of the text under the centre cover: dots, author, title, and the history line.
// The history line comes last and is the one that runs out of room -- the X3 panel is 8 px
// shorter than the X4's -- so there is a tight spacing to fall back on before giving it up. It
// buys 14 px from the gaps and from the title face's headroom: inter_ui_12 has ~7 px of accent
// room above its caps, so a pitch 2 px under the line height still leaves the ink 5 px apart.
struct CenterTextSpacing {
  int coverToDots;
  int dotsToAuthor;
  int authorToTitle;
  int titleToHistory;
};
constexpr CenterTextSpacing kRelaxedSpacing{8, 6, 2, 3};
constexpr CenterTextSpacing kTightSpacing{5, 2, -2, 0};

struct CenterTextLayout {
  int dotsY;
  int authorY;
  int titleY;
  int historyY;
};
CenterTextLayout layoutCenterText(const GfxRenderer& renderer, const int coverBottom, const CenterTextSpacing& gaps) {
  const int lineH = renderer.getLineHeight(kTitleFontId);
  CenterTextLayout l{};
  l.dotsY = coverBottom + gaps.coverToDots;
  l.authorY = l.dotsY + kDotSize + gaps.dotsToAuthor;
  l.titleY = l.authorY + lineH + gaps.authorToTitle;
  l.historyY = l.titleY + lineH + gaps.titleToHistory;
  return l;
}

int lastCarouselSelectorIndex = -1;

const uint8_t* iconBitmapFor(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Stats:
      return StatsIcon;
    case UIIcon::Ellipsis:
      return EllipsisIcon;
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
    default:
      return nullptr;
  }
}
// ---------------------------------------------------------------------------
// Static frame cache — survives HomeActivity re-creation so that returning to
// home after settings doesn't re-read covers from SD.
// Freed explicitly via invalidateFrameCache() before entering the reader.
//
// Only the cover tile region is cached, not the full framebuffer.  The header,
// menu, and button hints are always regenerated in tryFastHomeRender anyway, so
// caching them wastes ~10 KB.  Restore path: clearScreen() (white) then
// copyBufferToRegion() to paint the covers back.
//
// Cover region: x=0, y=(homeTopPadding - kCenterOutlineW), w=screenWidth,
// h=homeCoverTileHeight + kCenterOutlineW + dots + title lines + margin.
// The byte size is queried at runtime from the renderer so it adapts to X3/X4.
// ---------------------------------------------------------------------------
constexpr int kFrameCount = 1;
uint8_t* gCachedFrames[kFrameCount] = {};
int gCachedFrameBookIdx[kFrameCount] = {-1};
size_t gCachedFrameBytes = 0;  // region byte size (set on first allocation)
int gCachedFrameRegionY = 0;   // region top (logical)
int gCachedFrameRegionH = 0;   // region height (logical)
// Dirty flag: set when a new cover BMP was written to disk but the cached frame
// still shows the old placeholder.  Checked at the top of tryFastHomeRender so
// multiple covers arriving between renders coalesce into a single cache rebuild
// instead of triggering one SD re-read per cover.
bool gFrameCacheDirty = false;
int gCachedFrameCount = 0;
std::string gCacheKey;
// Book set whose region allocation already failed. The cached region is ~48 KB, the
// same order as the secondary framebuffer, and Home's steady-state free heap is
// smaller than either (measured on X3: 39,276 free vs 49,104 needed) — so while the
// secondary buffer is resident this malloc CANNOT succeed, and freeFrameCache()
// clearing gCacheKey made every subsequent render attempt it again. Remember the key
// that failed and skip the retry until something actually changes: a new book set, an
// explicit invalidate, or a cover landing on disk (markFrameCacheDirty). That keeps the
// cold-cache path intact — covers arriving there re-arm this on every decode — while a
// warm boot stops paying a doomed 48 KB allocation attempt per render.
std::string gAllocFailedKey;
// A lent region (HomeActivity's secondary framebuffer, after its cover pass) the one cached frame
// lives in instead of the heap. Null: frames come from malloc, as before.
uint8_t* gFrameRegion = nullptr;
size_t gFrameRegionBytes = 0;
static_assert(kFrameCount == 1, "the lent region holds exactly one frame");

uint8_t* allocFrame(const size_t bytes) {
  if (gFrameRegion != nullptr && gFrameRegionBytes >= bytes) return gFrameRegion;
  return static_cast<uint8_t*>(malloc(bytes));
}

// The cached strip: full width, from just above the centre cover's outline to just below the
// author/title lines.
struct FrameRegion {
  int y;
  int h;
};
FrameRegion carouselFrameRegion(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return FrameRegion{metrics.homeTopPadding - kCenterOutlineW, metrics.homeCoverTileHeight + kCenterOutlineW + 8 +
                                                                   kDotSize + 6 + renderer.getLineHeight(kTitleFontId) +
                                                                   2 + renderer.getLineHeight(kTitleFontId) + 4};
}

int findFrameSlot(int bookIdx) {
  for (int i = 0; i < kFrameCount; ++i) {
    if (gCachedFrameBookIdx[i] == bookIdx && gCachedFrames[i] != nullptr) return i;
  }
  return -1;
}

void freeFrameCache() {
  for (int i = 0; i < kFrameCount; ++i) {
    if (gCachedFrames[i]) {
      if (gCachedFrames[i] != gFrameRegion) free(gCachedFrames[i]);
      gCachedFrames[i] = nullptr;
    }
    gCachedFrameBookIdx[i] = -1;
  }
  gCachedFrameBytes = 0;
  gCachedFrameCount = 0;
  gFrameCacheDirty = false;
  gCacheKey.clear();
}
}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------
void LyraCarouselTheme::setPreRenderIndex(int idx) { lastCarouselSelectorIndex = idx; }

void LyraCarouselTheme::invalidateFrameCache() {
  freeFrameCache();
  gAllocFailedKey.clear();  // conditions may have changed; allow one more attempt
}
void LyraCarouselTheme::markFrameCacheDirty() {
  gFrameCacheDirty = true;
  gAllocFailedKey.clear();  // a cover just landed, so the rebuild is worth retrying
}

size_t LyraCarouselTheme::frameCacheRegionBytes(const GfxRenderer& renderer) const {
  const FrameRegion region = carouselFrameRegion(renderer);
  return renderer.getRegionByteSize(0, region.y, renderer.getScreenWidth(), region.h);
}

void LyraCarouselTheme::setFrameCacheRegion(uint8_t* region, const size_t bytes) {
  freeFrameCache();  // a frame in the old storage is gone either way
  gAllocFailedKey.clear();
  gFrameRegion = region;
  gFrameRegionBytes = region != nullptr ? bytes : 0;
}

void LyraCarouselTheme::onBookWillClose(const std::string& /*path*/, Epub* /*epub*/, Xtc* xtc, Txt* /*txt*/) {
  // EPUB thumbnail generation is handled lazily by HomeActivity (sliced ZIP + PNG decode).
  // XTC covers are generated synchronously here — they read from the first page of the XTC
  // file directly, no ZIP involved, so the extraction is fast enough to run at close time.
  if (xtc) {
    xtc->generateThumbBmp(kCenterCoverMaxW, kCenterCoverMaxH);
    xtc->generateThumbBmp(kSideCoverMaxW, kSideCoverMaxH);
  }
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

  renderer.copyRegionToBuffer(0, gCachedFrameRegionY, pageWidth, gCachedFrameRegionH, gCachedFrames[slotIdx],
                              gCachedFrameBytes);
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
  const uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;

  // Cover tile region: full width, from just above the centre cover outline to
  // just below the author/title text.  Everything outside this region is always
  // white — clearScreen() provides the background; header/menu/hints are drawn
  // fresh every call — so we only need to cache the cover tile itself.
  // This saves ~10 KB vs caching the full framebuffer (~52 KB on X3, ~48 KB on X4).
  const FrameRegion frameRegion = carouselFrameRegion(renderer);
  const int regionY = frameRegion.y;
  const int regionH = frameRegion.h;
  const size_t regionBytes = renderer.getRegionByteSize(0, regionY, pageWidth, regionH);

  // Build cache key from book paths
  std::string newKey;
  newKey.reserve(128);
  for (const auto& b : recentBooks) {
    newKey += b.path;
    newKey += '\0';
  }

  // If covers were written to disk since the last render, free the stale cached
  // frame so the rebuild path below picks up the new BMPs.  Multiple dirty marks
  // between renders coalesce here into one single rebuild.
  if (gFrameCacheDirty) {
    freeFrameCache();  // sets gFrameCacheDirty = false
  }

  if (newKey != gCacheKey || gCachedFrameCount == 0) {
    // Already established that this book set does not fit alongside whatever else is
    // resident; don't re-run the allocation (and its log line) on every render.
    if (newKey == gAllocFailedKey) return false;
    // Free old cache and allocate fresh region buffers.
    freeFrameCache();
    if (regionBytes == 0) return false;
    gCachedFrameRegionY = regionY;
    gCachedFrameRegionH = regionH;
    gCachedFrameBytes = regionBytes;
    const int frameCount = std::min(bookCount, kFrameCount);
    for (int i = 0; i < frameCount; ++i) {
      gCachedFrames[i] = allocFrame(regionBytes);
      if (!gCachedFrames[i]) {
        LOG_DBG("CAROUSEL",
                "tryFastHomeRender: malloc failed for cover region %d (%u bytes, %u free) — skipping until the book "
                "set changes or a cover lands",
                i, static_cast<unsigned>(regionBytes), static_cast<unsigned>(esp_get_free_heap_size()));
        freeFrameCache();  // clears gCacheKey, so latch AFTER it
        gAllocFailedKey = newKey;
        return false;
      }
    }
    // Render only the initially-selected frame; neighbours are filled lazily.
    const int initialIdx = (selectorIndex < bookCount) ? selectorIndex : 0;
    renderOneCarouselFrame(renderer, recentBooks, initialIdx, 0, metrics);
    gCachedFrameCount = frameCount;
    gCacheKey = newKey;
    gAllocFailedKey.clear();  // it fits now; forget any earlier failure
  }

  const bool inCarouselRow = (selectorIndex < bookCount);
  const int centerIdx =
      inCarouselRow ? selectorIndex : (lastCarouselSelectorIndex >= 0 ? lastCarouselSelectorIndex : 0);
  int slotIdx = findFrameSlot(centerIdx);
  if (slotIdx < 0) {
    slotIdx = 0;
  }
  if (!gCachedFrames[slotIdx]) {
    gCachedFrames[slotIdx] = allocFrame(gCachedFrameBytes);
    if (!gCachedFrames[slotIdx]) {
      LOG_DBG("CAROUSEL", "tryFastHomeRender: malloc failed for cover region %d — retrying next render", slotIdx);
      return false;
    }
  }
  if (findFrameSlot(centerIdx) < 0 || gCachedFrameBookIdx[slotIdx] != centerIdx) {
    renderOneCarouselFrame(renderer, recentBooks, centerIdx, slotIdx, metrics);
  }

  // Restore: clear screen to white, then paint the cached cover region back.
  renderer.clearScreen();
  renderer.copyBufferToRegion(0, gCachedFrameRegionY, pageWidth, gCachedFrameRegionH, gCachedFrames[slotIdx],
                              gCachedFrameBytes);
  UITheme::getInstance().getTheme().drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                                               nullptr);

  // The covers came back from the cache rather than from drawRecentBookCover(), so record their
  // targets here or the carousel would be the one home screen whose covers ignore a tap.
  recordCarouselCoverTargets(renderer.getScreenWidth(), renderer.getScreenHeight(), metrics.homeTopPadding, centerIdx,
                             bookCount);

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
  recordCarouselCoverTargets(screenW, renderer.getScreenHeight(), rect.y, centerIdx, bookCount);
  // Same slots the targets were just recorded from, so what is painted and what answers a tap
  // are one decision rather than two that agree by inspection.
  const CarouselCoverLayout::Slots slots =
      CarouselCoverLayout::compute(kCoverDims, screenW, rect.y, centerIdx, bookCount);
  const int centerTileY = slots.centre.y;
  const int sideTileY = slots.left.y;

  const int centerX = slots.centre.x;
  const int leftX = slots.left.x;
  const int rightX = slots.right.x;

  // Returns true if a book exists at bookIdx (cover image or placeholder drawn).
  // Returns false only when the slot has no book — caller skips the border too.
  // anyPending is set to true when a book has a coverBmpPath but the BMP isn't readable yet.
  bool anyPending = false;
  auto drawCover = [&](int bookIdx, int x, int y, int maxW, int maxH) -> bool {
    if (bookIdx < 0 || bookIdx >= bookCount) return false;
    const RecentBook& book = recentBooks[bookIdx];
    // Side tiles may extend off-screen — only round corners that are on-screen.
    const bool roundLeft = (x >= 0);
    const bool roundRight = (x + maxW <= screenW);
    bool hasCover = false;
    bool tilePending = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, maxW, maxH);
      FsFile file;
      if (Storage.openFileForRead("HOME", thumbPath, file)) {
        Bitmap bitmap(file);
        // The no-cover marker must never be drawn — scaling a 1x1 BMP into the tile would paint a
        // solid block. Leaving hasCover/tilePending false drops through to the no-cover tile below,
        // which is a better picture than the blank box the old slot-sized placeholder produced.
        if (bitmap.parseHeaders() == BmpReaderError::Ok &&
            !UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
          // Height always fills the tile. Only crop horizontally if the cover is
          // wider than the tile; narrow covers get white space on the sides.
          const float bmpRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float tileRatio = static_cast<float>(maxW) / static_cast<float>(maxH);
          const float cropX = (bmpRatio > tileRatio) ? (1.0f - tileRatio / bmpRatio) : 0.0f;
          renderer.fillRect(x, y, maxW, maxH, false);
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
      } else {
        tilePending = true;  // path exists but BMP not ready yet
        anyPending = true;
      }
    }
    if (!hasCover) {
      renderer.drawRoundedRect(x, y, maxW, maxH, 1, kCornerRadius, roundLeft, roundRight, roundLeft, roundRight, true);
      if (tilePending) {
        // Cover is being generated — show a loading label centred in the tile
        const char* loadingText = tr(STR_LOADING);
        const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
        const int textH = renderer.getLineHeight(SMALL_FONT_ID);
        renderer.drawText(SMALL_FONT_ID, x + (maxW - textW) / 2, y + (maxH - textH) / 2, loadingText, true);
      } else {
        renderer.fillRoundedRect(x, y + maxH / 3, maxW, 2 * maxH / 3, kCornerRadius, /*roundTopLeft=*/false,
                                 /*roundTopRight=*/false, /*roundBottomLeft=*/roundLeft,
                                 /*roundBottomRight=*/roundRight, Color::Black);
        renderer.drawIcon(CoverIcon, x + maxW / 2 - 16, y + 8, 32, 32);
        // Name the book inside the tile. The centre tile also carries author/title underneath, but
        // the two SIDE tiles have no label at all — without this a coverless book is unidentifiable
        // until it is scrolled into the centre. Drawn in the white upper third, under the icon.
        const int titleY = y + 8 + 32 + 4;
        const int titleLineH = renderer.getLineHeight(SMALL_FONT_ID);
        // Keep inside the white band: the black block starts at maxH/3.
        if (titleY + titleLineH <= y + maxH / 3) {
          const std::string tileTitle = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), maxW - 8);
          const int tileTitleW = renderer.getTextWidth(SMALL_FONT_ID, tileTitle.c_str());
          renderer.drawText(SMALL_FONT_ID, x + (maxW - tileTitleW) / 2, titleY, tileTitle.c_str(), true);
        }
      }
    }
    return true;
  };

  if (!coverRendered) {
    lastCarouselSelectorIndex = centerIdx;

    // Clear from the top of the tile down through the author/title text area.
    // Use absolute coordinates so the clear covers the text regardless of what
    // rect.height HomeActivity computed (it may be smaller than homeCoverTileHeight).
    // The relaxed spacing is the taller of the two, so it bounds whichever one gets drawn.
    const int textAreaBottom =
        layoutCenterText(renderer, centerTileY + kCenterCoverMaxH, kRelaxedSpacing).historyY  // history line top
        + renderer.getLineHeight(SMALL_FONT_ID)                                               // history line
        + 4;                                                                                  // small margin
    renderer.fillRect(rect.x, rect.y, rect.width, textAreaBottom - rect.y, false);

    // Sides first so centre renders on top.
    // Left side only when there are 3+ books; right side when there are 2+ books.
    // Border only drawn if a cover image was actually rendered (no placeholders).
    // bookIndex is -1 for a slot the carousel does not show at this position, and drawCover
    // rejects that -- the same test the recorder made, from the same computation.
    if (drawCover(slots.left.bookIndex, leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH)) {
      const bool rl = (leftX >= 0), rr = (leftX + kSideCoverMaxW <= screenW);
      renderer.drawRoundedRect(leftX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, 1, kCornerRadius, rl, rr, rl, rr,
                               true);
    }
    if (drawCover(slots.right.bookIndex, rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH)) {
      const bool rl = (rightX >= 0), rr = (rightX + kSideCoverMaxW <= screenW);
      renderer.drawRoundedRect(rightX, sideTileY, kSideCoverMaxW, kSideCoverMaxH, 1, kCornerRadius, rl, rr, rl, rr,
                               true);
    }

    // Clear a white outline ring around the centre cover, then draw the cover
    // inside it. The white ring always separates the centre from the sides.
    renderer.fillRect(centerX - kCenterOutlineW, centerTileY - kCenterOutlineW, kCenterCoverMaxW + 2 * kCenterOutlineW,
                      kCenterCoverMaxH + 2 * kCenterOutlineW, false);
    drawCover(centerIdx, centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH);

    // What you have put into this book, under what it is. The badge on the cover says what is
    // left of it; this says what it cost so far. A smaller face on purpose: it is a footnote to
    // the book, not a third thing competing with the title.
    //
    // Centre tile only. The side tiles are thumbnails of where you are going, and a row of
    // numbers under each would be noise. Empty for a book never opened, and then nothing is
    // drawn -- the space above is still cleared, so no ghost of a previous book's line remains.
    //
    // Measured against the SCREEN, not the cover. The author and title above are held to the
    // cover's width so they sit visually inside it, but this line is a sentence rather than a
    // label: holding it to 340px on a 480px panel threw away 70px a side and cut "last 1m ago"
    // to "last 1...". Centred on the screen, which is where the cover is centred anyway.
    const RecentBook& centerBook = recentBooks[centerIdx];
    const std::string history = BookProgressPresentation::historyLine(centerBook);
    const int historyMaxW = screenW - kHistorySideMargin * 2;
    // Drop a size before clipping, the same order the button hints use: this is a sentence, and
    // half a sentence tells you less than the whole one a little smaller. FIT_SMALL_FONT_ID
    // does not move with the UI font setting, which is what makes it a floor to fall back to.
    const int historyFont =
        renderer.getTextWidth(SMALL_FONT_ID, history.c_str()) > historyMaxW ? FIT_SMALL_FONT_ID : SMALL_FONT_ID;

    // It has to end above the icon row, which is anchored to the bottom of the screen, so how
    // much room it gets depends on the panel height and the UI font size. Relaxed spacing if it
    // fits, tight spacing if that makes it fit, and otherwise it does not go under the cover at
    // all: its compact form joins the badge on the cover instead.
    const int coverBottom = centerTileY + kCenterCoverMaxH;
    CenterTextLayout text = layoutCenterText(renderer, coverBottom, kRelaxedSpacing);
    bool historyUnderCover = false;
    if (!history.empty()) {
      const int floorY = menuInkTop(renderer);
      const int historyH = renderer.getLineHeight(historyFont);
      for (const CenterTextSpacing* gaps : {&kRelaxedSpacing, &kTightSpacing}) {
        const CenterTextLayout candidate = layoutCenterText(renderer, coverBottom, *gaps);
        if (candidate.historyY + historyH <= floorY) {
          text = candidate;
          historyUnderCover = true;
          break;
        }
      }
    }

    // Progress + pace-based ETA as a badge in the centre cover's top-right corner,
    // e.g. "62% · ~45m". The large centre cover has room to carry it overlaid.
    const int centerProgress = BookProgressPresentation::readPercent(centerBook);
    BookProgressPresentation::drawBadge(
        renderer, Rect{centerX, centerTileY, kCenterCoverMaxW, kCenterCoverMaxH}, centerBook, centerProgress,
        historyUnderCover ? std::string{} : BookProgressPresentation::historyLineCompact(centerBook));

    // Dots — centred over the cover tile, count = actual book count
    const int totalDotsW = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = centerX + (kCenterCoverMaxW - totalDotsW) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIdx)
        renderer.fillRect(dotX, text.dotsY, kDotSize, kDotSize, true);
      else
        renderer.drawRect(dotX, text.dotsY, kDotSize, kDotSize, true);
      dotX += kDotSize + kDotGap;
    }

    // Author then title below dots
    const std::string authorTrunc = renderer.truncatedText(kTitleFontId, centerBook.author.c_str(), kCenterCoverMaxW);
    const int authorW = renderer.getTextWidth(kTitleFontId, authorTrunc.c_str());
    renderer.drawText(kTitleFontId, centerX + (kCenterCoverMaxW - authorW) / 2, text.authorY, authorTrunc.c_str(),
                      true);

    const std::string titleTrunc = renderer.truncatedText(kTitleFontId, centerBook.title.c_str(), kCenterCoverMaxW);
    const int titleW = renderer.getTextWidth(kTitleFontId, titleTrunc.c_str());
    renderer.drawText(kTitleFontId, centerX + (kCenterCoverMaxW - titleW) / 2, text.titleY, titleTrunc.c_str(), true);

    if (historyUnderCover) {
      const std::string historyTrunc = renderer.truncatedText(historyFont, history.c_str(), historyMaxW);
      const int historyW = renderer.getTextWidth(historyFont, historyTrunc.c_str());
      renderer.drawText(historyFont, (screenW - historyW) / 2, text.historyY, historyTrunc.c_str(), true);
    }

    // Only cache the frame buffer once all tiles are definitively resolved.
    // If any cover is still being generated we keep coverRendered=false so the next render will retry.
    if (!anyPending) {
      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;
    }
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
  const int tileH = kMenuTileH;
  // Anchor row just above button hints, ignoring rect.y which may be off-screen for large cover tiles
  const int rowY = menuRowY(renderer);

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

  // Rows published for touch — see BaseTheme::drawButtonMenu. This theme's menu is a HORIZONTAL
  // icon strip, which is why these are free rects rather than a ListTouchBand: no y-band model
  // describes both this and the vertical menus. The whole tile is the target, not just the icon,
  // so a finger does not have to land on a 32 px glyph. Values are MENU-LOCAL indices, and the
  // sliding window means slot != index whenever the icons overflow.
  TapTargets::Recorder::Builder menuTargets;

  for (int slot = 0; slot < visibleCount; ++slot) {
    const int i = firstVisible + slot;
    if (i >= buttonCount) break;

    const int tileX = slot * tileW;
    const int iconX = tileX + (tileW - kMenuIconSize) / 2;
    const int iconY = rowY + kMenuIconPad;
    menuTargets.add(tileX, rowY, tileW, tileH, i);

    const bool selected = (selectedIndex == i);
    if (selected) {
      const int highlightSize = kMenuHighlightSize;
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

  TapTargets::homeMenu().record(menuTargets);
}

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

  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const UiFontLadder::Step growth = UITheme::fontGrowth();
  const int rowHeight = (rowSubtitle != nullptr) ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  // Never paint more rows than the touch band can register — see BaseTheme::drawList.
  const int pageItems = std::min(rect.height / rowHeight, ListTouchBand::kMaxRows);
  if (pageItems <= 0 || itemCount <= 0 || rowTitle == nullptr) {
    ListTouchBand::invalidate();
    return;
  }
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
  int iconY = 0;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSz : listIconSz;
    iconY = (rowHeight - iconSize) / 2;
    textX += iconSize + hPad;
    textWidth -= iconSize + hPad;
  }

  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  // Rows published for touch as they are painted — see BaseTheme::drawList. This theme is the
  // third fixed-height list draw, and the one that shipped without this: every screen drawn
  // through GUI.drawList was painted correctly and answered no tap at all.
  ListTouchBand::Builder touchBand;
  touchBand.begin(rect.x, rect.width, pageStartIndex);

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
    if (i == selectedIndex) SerialTestApi::noteSelection(i, itemName.c_str());  // CMD:STATE sel=, label=
    const bool isSeparator = UITheme::isSeparatorTitle(itemName);
    touchBand.addRow(itemY, rowHeight, !isSeparator);
    if (isSeparator) {
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
      // Pushed down by however much the title line above it grew at the current UI font size.
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30 + growth.body, subtitle.c_str(), !sel);
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

  touchBand.commit();
}
