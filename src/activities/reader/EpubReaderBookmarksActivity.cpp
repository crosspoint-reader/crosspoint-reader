#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "../../util/BookmarkFile.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::shared_ptr<Epub>& epub, const std::string& epubPath)
    : UiListActivity("EpubReaderBookmarks", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      epub(epub),
      epubPath(epubPath) {}

void EpubReaderBookmarksActivity::onEnter() {
  UiListActivity::onEnter();

  if (!epub) {
    return;
  }

  if (!BookmarkFile::load(epubPath, bookmarks)) {
    bookmarks.shrink_to_fit();
  }
  LOG_DBG("EPB", "Loaded %d bookmarks for book: %s", static_cast<int>(bookmarks.size()), epubPath.c_str());
  rebuildBookmarkRowItems();
}

// Derives bookmarkSubtitles/bookmarkRowItems from `bookmarks`. Called
// whenever `bookmarks` changes (onEnter() load, post-delete) so buildScreen()
// reuses the cached rows on every repaint instead of re-composing a
// percentage/chapter/TOC-title subtitle string per bookmark each time.
void EpubReaderBookmarksActivity::rebuildBookmarkRowItems() {
  bookmarkSubtitles.clear();
  bookmarkRowItems.clear();
  if (!epub) {
    return;
  }
  bookmarkSubtitles.reserve(bookmarks.size());
  bookmarkRowItems.reserve(bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    const auto tocIndex = epub->getTocIndexForSpineIndex(bookmark.computedSpineIndex);
    const auto tocTitle = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : tr(STR_UNNAMED);
    std::string subtitle = std::to_string((int)(std::clamp(bookmark.percentage, 0.0f, 1.0f) * 100.0f + 0.5f)) + "% - ";
    if (bookmark.computedChapterPageCount > 0) {
      subtitle += std::to_string(bookmark.computedChapterProgress + 1) + "/" +
                  std::to_string(bookmark.computedChapterPageCount) + " - ";
    }
    subtitle += tocTitle;
    bookmarkSubtitles.push_back(std::move(subtitle));

    fui::ListItem item;
    item.label = bookmark.summary.c_str();
    item.subtitle = bookmarkSubtitles.back().c_str();
    item.icon = listIconFor(UIIcon::Bookmark, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(bookmarkRowItems.size());
    bookmarkRowItems.push_back(item);
  }
}

void EpubReaderBookmarksActivity::openSelectedBookmark() {
  if (bookmarks.empty()) {
    return;
  }
  const auto& bookmark = bookmarks.at(nav.selected);
  ProgressChangeResult result{};
  result.xpath = bookmark.xpath;
  result.percentage = bookmark.percentage;
  result.hasSavedProgress = true;
  result.hasVisibleTextOffset = bookmark.hasVisibleTextOffset;
  result.visibleTextOffset = bookmark.visibleTextOffset;
  // The offset is spine-relative, so carry its spine even when the legacy page hints below
  // are stale. The reader validates the index.
  result.spineIndex = bookmark.computedSpineIndex;
  if (bookmark.computedChapterPageCount > 0 && bookmark.computedChapterProgress < bookmark.computedChapterPageCount &&
      bookmark.computedSpineIndex < epub->getSpineItemsCount()) {
    result.page = bookmark.computedChapterProgress;
    result.totalPages = bookmark.computedChapterPageCount;
  }
  setResult(std::move(result));
  finish();
}

void EpubReaderBookmarksActivity::activateIndex(const int index) {
  if (confirmPopup.isActive()) return;
  // The interaction table can deliver a row index captured before a delete
  // shrank the list; the next render re-registers the rows.
  if (index < 0 || index >= listCount()) return;
  // The tapped row leaves this screen; a lingering flash would gray an
  // unrelated row on the next render.
  app.clearTapFlash();
  nav.selected = index;
  openSelectedBookmark();
}

void EpubReaderBookmarksActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  // The row is deleted; a lingering flash would gray an unrelated row on the
  // next render.
  app.clearTapFlash();
  nav.selected = index;
  // Touch long-press asks the same Cancel/Delete confirmation as the physical
  // hold (the popup is tap-operable), matching the file browser's long-press
  // delete flow. Does not open the bookmark.
  showDeleteConfirmation();
}

bool EpubReaderBookmarksActivity::handleCustomInput() {
  // Delete confirmation popup
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    // Popup dismissed without a selection (Back button or tap outside): cancel delete
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderBookmarksActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
      showDeleteConfirmation();
    } else {
      openSelectedBookmark();
    }
    return true;
  }

  return false;
}

void EpubReaderBookmarksActivity::showDeleteConfirmation() {
  if (bookmarks.empty() || confirmPopup.isActive()) {
    return;
  }
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_CONFIRM_DELETE_BOOKMARK), options, 2, 0, [this](int idx) {
    confirmingDelete = false;
    if (idx == 1) {
      deleteSelectedBookmark();
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  bookmarks.erase(bookmarks.begin() + nav.selected);
  // Deleting shifts every later bookmark's index, so the cached subtitles and
  // actionValues must be re-derived, not just trimmed — and before the SD
  // save, so the render task never sees rows aliasing the erased storage.
  rebuildBookmarkRowItems();
  if (!BookmarkFile::save(epubPath, bookmarks)) {
    LOG_ERR("EPB", "Failed to save bookmarks after delete");
  }

  // Move selector up if we deleted the last item
  if (nav.selected >= static_cast<int>(bookmarks.size()) && nav.selected > 0) {
    nav.selected--;
  }

  if (bookmarks.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  nav.follow(listCount());
  requestUpdate(true);
}

void EpubReaderBookmarksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the title band render() paints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (bookmarks.empty()) {
    screen.centeredText(tr(STR_NO_BOOKMARKS), screen.theme().bodyText);
    return;
  }

  // "Hold Open to Delete" names a physical button; on touch boards the row
  // long-press covers deletion, so the hint would be wrong there.
  if (!mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_OPEN_TO_DELETE));
  }

  // bookmarkSubtitles/bookmarkRowItems are built once whenever `bookmarks`
  // changes (see rebuildBookmarkRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = bookmarkRowItems.data();
  props.count = static_cast<uint16_t>(bookmarkRowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press deletes (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;

  // Manual centering to honor content gutters.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  renderUi();

  if (confirmPopup.processRender(renderer, mappedInput)) return;

  const auto confirmLabel = bookmarks.size() > 0 ? tr(STR_SELECT) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
