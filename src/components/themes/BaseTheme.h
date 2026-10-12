#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "components/themes/HintLayout.h"

class GfxRenderer;
class Epub;
class Txt;
class Xtc;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

// How drawPopup() ships the frame it just composed.
enum class PopupShip : uint8_t {
  Blocking,  // displayBuffer(): return once the panel has finished painting the box.
  Async,     // triggerDisplayAsync(): return while the panel is still painting, so the caller can
             // start the slow work the popup is announcing. The caller then owes the panel a
             // GfxRenderer::finishDisplayAsync() before it next writes the framebuffer, touches
             // the display, or frees a framebuffer — releaseSecondaryBuffer() in particular does
             // NOT drain a refresh in flight, and X3 re-reads the frame after the waveform.
  Caller,    // Not shipped: the box is only drawn into the write buffer, and the caller's own
             // displayBuffer() ships it with the rest of the frame. For a popup drawn inside a
             // render that ships itself, such as UiListActivity::render() via drawFooter(): a ship
             // here would swap the buffers, and the render's displayBuffer() would then ship the
             // stale one. Pass overlayDisplayedFrame=false with it, since the caller composed the frame.
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu = false;
  int homeMenuTopOffset = 0;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;

  int keyboardKeyWidth;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardKeyCornerRadius = 0;
};

enum UIIcon { Folder, Text, Image, Book, File, Recent, Settings, Transfer, Library, Weather, Stats, Ellipsis };

enum class HomeNavigation { Linear, Carousel };

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 65,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85};
}

enum class SyncIndicator : uint8_t { None, Active, Failed };

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  virtual void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  virtual void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                               bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                                bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(GfxRenderer& renderer, const char* upBtn, const char* downBtn) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr,
                        bool highlightValue = false) const;
  virtual void drawListSeparator(const GfxRenderer& renderer, Rect rowRect, int textX, int textWidth,
                                 const std::string& title) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  // Draws a centered message box and ships it. By default the box is overlaid on the frame that is
  // currently on screen: displayBuffer() ends in a buffer swap, so the write buffer holds the frame
  // from two refreshes ago, and a bare overlay would diff stale content around the box (ghosting).
  // Syncing the write buffer from the displayed frame first fixes that. Callers that have already
  // composed a full fresh frame into the write buffer (clearScreen + render, then popup in the same
  // displayBuffer) must pass overlayDisplayedFrame=false so their render is not discarded.
  // ship=Async returns while the panel paints; see PopupShip for what the caller then owes.
  // ship=Caller draws without shipping; the caller's displayBuffer() ships the frame.
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, bool overlayDisplayedFrame = true,
                         PopupShip ship = PopupShip::Blocking) const;
  // Screen-centred, icon-only "working on it" box: an hourglass, no words. Deliberately wordless —
  // a transition indicator is glanced at, not read, so text draws the eye for no added meaning and
  // would need translating besides. Same frame, sync rule and ship contract as drawPopup(),
  // including what PopupShip::Async leaves the caller owing.
  virtual Rect drawBusyIndicator(const GfxRenderer& renderer, bool overlayDisplayedFrame = true,
                                 PopupShip ship = PopupShip::Blocking) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  virtual void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                             const int pageCount, std::string title, const int paddingBottom = 0,
                             const bool isStarred = false, const std::string& printedPageLabel = std::string(),
                             const bool fillMargin = true, const bool pageCountApproximate = false,
                             const SyncIndicator syncIndicator = SyncIndicator::None) const;
  virtual void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }

  // ---- Home screen navigation / rendering contract ----

  // Return Carousel to opt in to left/right book navigation and tryFastHomeRender().
  virtual HomeNavigation getHomeNavigation() const { return HomeNavigation::Linear; }

  // Return the cover thumbnail sizes this theme needs for its home screen.
  // HomeActivity calls generateThumbBmp() for each pair. Empty = use height-only path.
  virtual std::vector<std::pair<int, int>> getCoverThumbSizes(int coverHeight) const { return {}; }

  // Attempt a full home-screen render from pre-cached state.
  // Return true if the theme handled the render (HomeActivity must not draw anything else).
  // Return false to fall through to the standard slow-path render in HomeActivity.
  virtual bool tryFastHomeRender(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                 int menuCount, const std::function<std::string(int)>& menuLabel,
                                 const std::function<UIIcon(int)>& menuIcon, const char* hintBtn1, const char* hintBtn2,
                                 const char* hintBtn3, const char* hintBtn4) const {
    return false;
  }

  // Called by readers just before releasing the book object. Themes that cache
  // cover thumbnails can generate them here while the book is still loaded.
  // Only one of epub/xtc/txt will be non-null depending on the reader.
  virtual void onBookWillClose(const std::string& path, Epub* epub, Xtc* xtc, Txt* txt) {}

  // Called when HomeActivity exits. Themes that hold heap-allocated render caches
  // should free them here so the memory is available to child activities.
  virtual void invalidateFrameCache() {}

  // Mark the frame cache stale without freeing it. tryFastHomeRender will
  // rebuild on the next render pass. Multiple dirty marks coalesce into one
  // rebuild — use this instead of invalidateFrameCache() when a cover BMP was
  // just written so back-to-back cover arrivals don't each trigger an SD re-read.
  virtual void markFrameCacheDirty() {}

  // Bytes of lent memory the frame cache would live in instead of the heap; 0 = it keeps none.
  // HomeActivity keeps the secondary framebuffer lent after its cover pass and offers it here: the
  // carousel's one cached frame (49 104 B on the X3, 45 120 B on the X4) is larger than any block
  // Home's heap has while the buffer is resident, so on the heap it never fit.
  virtual size_t frameCacheRegionBytes(const GfxRenderer& /*renderer*/) const { return 0; }
  // Give the frame cache a lent region to live in, or take it back (null). Whatever frame the cache
  // held is dropped either way. Call under the RenderLock: tryFastHomeRender reads the cache on
  // the render task.
  virtual void setFrameCacheRegion(uint8_t* /*region*/, size_t /*bytes*/) {}

  // ---- Shared constants and helpers for battery drawing (used by all themes) ----
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

  // Footprint actually occupied by drawBatteryLeft(): the icon plus, when shown, the live
  // percentage text. Anything placed to its right (the clock, the title) must reserve this rather
  // than estimate it — a three digit percentage ("100%") is wider than any fixed guess, which is
  // how the clock ended up drawn on top of the battery percentage (issue #214).
  static int statusBarBatteryWidth(const GfxRenderer& renderer, const ThemeMetrics& metrics, bool showPercentage);

 protected:
  // Up/down triangles marking that the list continues past the visible rows. Used by themes
  // without a scroll bar.
  static void drawListOverflowArrows(const GfxRenderer& renderer, Rect rect);

  // Ships the frame a drawPopup() override just composed, blocking or not, or leaves it to the
  // caller (PopupShip::Caller). One place so the two popup looks cannot drift on the part that
  // isn't a look at all.
  static void shipPopup(const GfxRenderer& renderer, PopupShip ship);

  // ---- Hint box sizing, shared by every drawButtonHints() / drawSideButtonHints() ----
  // See HintLayout.h for the rule: a box grows to fit its label, then the label shrinks, then
  // it is clipped.

  // Width of `label` in `fontId`; 0 for a null or empty label, which needs no room.
  static int hintLabelWidth(const GfxRenderer& renderer, int fontId, const char* label);
  // `label` fitted to `maxWidth`: in `fontId` if it fits, else the default-size face, clipped
  // only if even that will not fit. A whole word a size smaller is still the word, where
  // "Down..." is a guess at which of Down and Download the button means.
  struct FittedHintLabel {
    int fontId;
    std::string text;
  };
  static FittedHintLabel fitHintLabel(const GfxRenderer& renderer, int fontId, const char* label, int maxWidth);

  // Where each bottom box goes and how wide it is: as wide as its label needs in `fontId`, up to
  // a fair quarter of the panel, laid out around the `tuned` positions. Empty labels keep `base`.
  static void layOutButtonHints(const GfxRenderer& renderer, int fontId, int base, const int (&tuned)[4],
                                const char* const (&labels)[4], int (&x)[4], int (&width)[4]);

  // The stretch of the side column the side boxes may grow into, in panel coordinates. Call it
  // BEFORE forcing the draw into the panel frame: where the header lies depends on the live
  // orientation.
  static HintLayout::Span sideHintSpan(const GfxRenderer& renderer);
  // Length the longer of the two side labels needs in `fontId`, padding included.
  static int sideHintNeededLength(const GfxRenderer& renderer, int fontId, const char* upLabel, const char* downLabel);
};
