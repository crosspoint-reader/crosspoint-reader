#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const uint8_t currentAutoPageTurnSeconds, const bool autoPageTurnActive,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool currentPageBookmarked, const ReaderKind readerKind,
                                               const bool canCreateClipping, const bool hasClippings,
                                               const bool hasChapters, const bool bookCompleted)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(readerKind, hasFootnotes, hasBookmarks, currentPageBookmarked, canCreateClipping,
                               hasClippings, hasChapters, bookCompleted)),
      title(title),
      pendingOrientation(currentOrientation),
      selectedAutoPageTurnSeconds(
          currentAutoPageTurnSeconds == 0 ? 0 : std::clamp<uint8_t>(currentAutoPageTurnSeconds, 5, 120)),
      selectedAutoPageTurnActive(autoPageTurnActive && selectedAutoPageTurnSeconds != 0),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      readerKind(readerKind) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(
    const ReaderKind readerKind, const bool hasFootnotes, const bool hasBookmarks, const bool currentPageBookmarked,
    const bool canCreateClipping, const bool hasClippings, const bool hasChapters, const bool bookCompleted) {
  std::vector<MenuItem> items;
  items.reserve(18);
  if (readerKind == ReaderKind::FixedLayout) {
    if (hasChapters) items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
    items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
    items.push_back({MenuAction::GO_TO_PAGE, StrId::STR_GO_TO_PAGE});
    items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL});
    if (!bookCompleted) items.push_back({MenuAction::MARK_COMPLETE, StrId::STR_MARK_BOOK_COMPLETE});
    items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
    items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
    return items;
  }
  if (readerKind == ReaderKind::PlainText) {
    items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  } else {
    items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
    items.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  }
  if (readerKind == ReaderKind::Epub && hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, currentPageBookmarked ? StrId::STR_REMOVE_BOOKMARK_THIS_PAGE
                                                                      : StrId::STR_BOOKMARK_THIS_PAGE});
  items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  if (canCreateClipping) items.push_back({MenuAction::CREATE_CLIPPING, StrId::STR_CREATE_CLIPPING});
  if (readerKind == ReaderKind::Epub || hasClippings) {
    items.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_CLIPPINGS});
  }
  items.push_back({MenuAction::BOOK_SETTINGS, StrId::STR_BOOK_READER_SETTINGS});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  if (readerKind == ReaderKind::Epub) items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  if (readerKind == ReaderKind::Epub) {
    items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
    items.push_back({MenuAction::NEARBY_POSITION_SYNC, StrId::STR_NEARBY_POSITION_SYNC});
  }
  items.push_back({MenuAction::DELETE_CACHE,
                   readerKind == ReaderKind::PlainText ? StrId::STR_CLEAR_READING_CACHE : StrId::STR_DELETE_CACHE});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      startActivityForResult(std::make_unique<IntervalSelectionActivity>(
                                 renderer, mappedInput, "AutoPageTurnInterval", StrId::STR_AUTO_TURN_INTERVAL,
                                 selectedAutoPageTurnSeconds, 0, 120, 5, 15, StrId::STR_AUTO_TURN_SECONDS_FORMAT, true,
                                 true, StrId::STR_NONE_OPT, StrId::STR_STATE_OFF),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const uint32_t selected = std::get<IntervalResult>(result.data).value;
                                 selectedAutoPageTurnSeconds =
                                     selected == 0 ? 0 : static_cast<uint8_t>(std::clamp<uint32_t>(selected, 5, 120));
                                 selectedAutoPageTurnActive = selectedAutoPageTurnSeconds != 0;
                                 autoPageTurnChanged = true;
                               }
                               requestUpdate();
                             });
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedAutoPageTurnSeconds,
                         autoPageTurnChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedAutoPageTurnSeconds, autoPageTurnChanged};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary
  std::string progressLine;
  if ((readerKind == ReaderKind::PlainText || readerKind == ReaderKind::FixedLayout) && totalPages > 0) {
    char formatted[80];
    snprintf(formatted, sizeof(formatted), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage, totalPages,
             static_cast<double>(bookProgressPercent));
    progressLine = formatted;
  } else if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
    progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  } else {
    progressLine = std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  }
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) -> std::string {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          return autoPageTurnValue();
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string EpubReaderMenuActivity::autoPageTurnValue() const {
  if (!selectedAutoPageTurnActive) return I18N.get(StrId::STR_STATE_OFF);
  char formatted[24];
  snprintf(formatted, sizeof(formatted), I18N.get(StrId::STR_AUTO_TURN_SECONDS_FORMAT),
           static_cast<unsigned>(selectedAutoPageTurnSeconds));
  return formatted;
}
