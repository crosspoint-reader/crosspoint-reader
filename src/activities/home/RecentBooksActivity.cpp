#include "RecentBooksActivity.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "MappedInputManager.h"
#include "RecentBookCoverPainter.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;

struct RecentBooksRects {
  Rect header;
  Rect list;
  Rect buttons;
  bool themed = false;
};

struct RecentBooksCoverGridItemProviderData {
  const std::vector<RecentBook>* recentBooks = nullptr;
};

freeink::ui::CoverGridItem provideRecentBooksCoverGridItem(uint16_t index, void* userData) {
  auto* data = static_cast<RecentBooksCoverGridItemProviderData*>(userData);
  if (data == nullptr || data->recentBooks == nullptr || index >= data->recentBooks->size()) return {};
  return freeink::ui::coverGridItem((*data->recentBooks)[index].title.c_str(), index);
}

const ThemeCoverGridWidgetSpec* recentBooksCoverGridWidget(const ThemeScreenSpec* screenSpec) {
  if (screenSpec == nullptr) return nullptr;
  const auto it = std::find_if(screenSpec->widgets.begin(), screenSpec->widgets.end(),
                               [](const auto& widget) { return widget.type == ThemeScreenWidgetType::CoverGrid; });
  return it == screenSpec->widgets.end() ? nullptr : &it->coverGrid;
}

ThemeCoverGridWidgetSpec normalizedRecentBooksCoverGridSpec(const ThemeCoverGridWidgetSpec& source) {
  ThemeCoverGridWidgetSpec spec = source;
  if (!spec.configured) {
    spec.columns = 3;
    spec.gap = 14;
    spec.rowGap = 20;
    spec.coverWidth = 92;
    spec.coverHeight = 132;
    spec.rowHeight = 172;
    spec.labelHeight = 34;
    spec.labelLines = 2;
    spec.selectedRadius = 0;
    spec.selectionStyle = ThemeWidgetSelectionStyle::CoverFrame;
    spec.cellInset.top = 5;
    spec.labelInset.left = 5;
    spec.labelInset.right = 5;
  }
  spec.columns = std::max(1, spec.columns);
  spec.rowGap = spec.rowGap >= 0 ? spec.rowGap : std::max(0, spec.gap);
  spec.coverHeight = spec.coverHeight > 0 ? spec.coverHeight : 132;
  spec.coverWidth = spec.coverWidth > 0 ? spec.coverWidth : std::max(1, spec.coverHeight * 62 / 100);
  spec.placeholderIconSize = std::max(0, spec.placeholderIconSize);
  spec.labelHeight = std::max(0, spec.labelHeight);
  spec.labelGap = std::max(0, spec.labelGap);
  spec.labelLines = std::max(1, std::min(3, spec.labelLines));
  spec.rowHeight = spec.rowHeight > 0 ? spec.rowHeight : spec.coverHeight + spec.labelHeight + 6;
  return spec;
}

RecentBooksRects resolveRecentBooksRects(GfxRenderer& renderer, const ThemeMetrics& metrics,
                                         const ThemeScreenSpec*& screenSpec) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  RecentBooksRects rects;

  if (screenSpec != nullptr) {
    ThemeLayoutSlots slots;
    layoutThemeSlots(screenSpec->layout, Rect{0, 0, pageWidth, pageHeight}, metrics, slots);
    rects.header = normalizeThemeHeaderSlot(findThemeSlot(slots, "header"), metrics);
    rects.list = findThemeSlot(slots, "list");
    rects.buttons = findThemeSlot(slots, "buttons");
    if (rects.list.width > 0 && rects.list.height > 0) {
      rects.themed = true;
      return rects;
    }
    LOG_ERR("RecentBooks", "Invalid SD recent layout: slots=%d; using built-in layout", static_cast<int>(slots.size()));
    screenSpec = nullptr;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  rects.header = Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight};
  rects.list = Rect{0, contentTop, pageWidth, contentHeight};
  rects.buttons = Rect{0, pageHeight - metrics.buttonHintsHeight, pageWidth, metrics.buttonHintsHeight};
  return rects;
}

int recentBooksCoverGridPageItems(Rect listRect, const ThemeCoverGridWidgetSpec& spec) {
  return std::max<int>(1, freeink::ui::coverGridVisibleCells(
                              freeink::ui::makeRect(listRect.x, listRect.y, listRect.width, listRect.height),
                              std::min<int>(std::max(1, spec.columns), 12), freeink::ui::clampI16(spec.rowHeight, 1),
                              freeink::ui::clampI16(spec.rowGap)));
}

freeink::ui::Insets toFreeInkInsets(const ThemeEdgeInsets& insets) {
  return freeink::ui::makeInsets(insets.top, insets.right, insets.bottom, insets.left);
}

freeink::ui::StyleSet recentBooksGridStyles(const ThemeCoverGridWidgetSpec& spec) {
  return freeink::ui::selectedOutlineListRowStyles(spec.selectedRadius);
}

}  // namespace

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

int RecentBooksActivity::getPageItems() {
  auto& theme = UITheme::getInstance();
  const ThemeScreenSpec* screenSpec = theme.getScreenSpec(ThemeScreenKind::RecentBooks);
  const auto rects = resolveRecentBooksRects(renderer, theme.getMetrics(), screenSpec);
  if (rects.themed) {
    const ThemeCoverGridWidgetSpec* grid = recentBooksCoverGridWidget(screenSpec);
    if (grid != nullptr) return recentBooksCoverGridPageItems(rects.list, normalizedRecentBooksCoverGridSpec(*grid));
  }
  return theme.getNumberOfItemsPerPage(renderer, true, false, true, true);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = getPageItems();

  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const ThemeScreenSpec* screenSpec = theme.getScreenSpec(ThemeScreenKind::RecentBooks);
  const auto rects = resolveRecentBooksRects(renderer, metrics, screenSpec);
  const ThemeCoverGridWidgetSpec* coverGridWidget = rects.themed ? recentBooksCoverGridWidget(screenSpec) : nullptr;

  if (rects.header.width > 0 && rects.header.height > 0) {
    GUI.drawHeader(renderer, rects.header, tr(STR_MENU_RECENT_BOOKS));
  }

  // Recent tab
  if (rects.list.width <= 0 || rects.list.height <= 0) {
    // Malformed theme layout: no list slot to draw into.
  } else if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, rects.list.y + 20, tr(STR_NO_RECENT_BOOKS));
  } else if (coverGridWidget != nullptr) {
#if FREEINK_HAVE_GFX_RENDERER
    const auto gridSpec = normalizedRecentBooksCoverGridSpec(*coverGridWidget);
    freeink::ui::GfxRendererFrame<> ui(renderer, SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID);
    RecentBookCoverPainterData painterData{
        &renderer, &recentBooks, UITheme::getInstance().getRecentBooksCoverThumbHeight(), gridSpec.placeholderIconSize};
    RecentBooksCoverGridItemProviderData itemProviderData{&recentBooks};
    const int pageItems = recentBooksCoverGridPageItems(rects.list, gridSpec);
    freeink::ui::CoverGridProps props;
    props.itemProvider = provideRecentBooksCoverGridItem;
    props.itemProviderUserData = &itemProviderData;
    props.count = static_cast<uint16_t>(std::min<size_t>(recentBooks.size(), 65535));
    props.topIndex = freeink::ui::coverGridTopIndexFor(
        static_cast<uint16_t>(selectorIndex), static_cast<uint16_t>(std::min<size_t>(recentBooks.size(), 65535)),
        std::min<int>(std::max(1, gridSpec.columns), 12), static_cast<uint16_t>(pageItems));
    props.selectedIndex = static_cast<int16_t>(selectorIndex);
    props.columns = static_cast<uint8_t>(std::min(std::max(1, gridSpec.columns), 12));
    props.gap = freeink::ui::clampI16(gridSpec.gap);
    props.rowGap = freeink::ui::clampI16(gridSpec.rowGap);
    props.cellInset = toFreeInkInsets(gridSpec.cellInset);
    props.labelInset = toFreeInkInsets(gridSpec.labelInset);
    props.coverSize = freeink::ui::makeSize(gridSpec.coverWidth, gridSpec.coverHeight);
    props.rowHeight = freeink::ui::clampI16(gridSpec.rowHeight, 1);
    props.labelHeight = freeink::ui::clampI16(gridSpec.labelHeight);
    props.labelGap = freeink::ui::clampI16(gridSpec.labelGap);
    props.titleText.font = freeink::ui::GfxRendererTarget::FONT_SMALL;
    props.titleText.maxLines = static_cast<uint8_t>(std::max(1, std::min(3, gridSpec.labelLines)));
    props.cellStyles = recentBooksGridStyles(gridSpec);
    props.selectionIndicator = freeink::ui::CoverGridSelectionIndicator::CoverFrame;
    props.selectedCoverFrameRadius = freeink::ui::clampRadius(gridSpec.selectedRadius);
    props.coverPainter = paintRecentCoverGridCover;
    props.coverPainterUserData = &painterData;
    freeink::ui::coverGrid(
        ui.frame, freeink::ui::makeRect(rects.list.x, rects.list.y, rects.list.width, rects.list.height), props);
#endif
  } else {
    GUI.drawList(
        renderer, rects.list, recentBooks.size(), selectorIndex, [this](int index) { return recentBooks[index].title; },
        [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  if (rects.buttons.width > 0 && rects.buttons.height > 0) {
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
