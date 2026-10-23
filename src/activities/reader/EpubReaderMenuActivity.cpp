#include "EpubReaderMenuActivity.h"

#include <BlePageTurner.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool hasClippings, MenuState& menuState)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput),
      menuState(menuState),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(menuItems, hasFootnotes, hasBookmarks, hasClippings);
}

void EpubReaderMenuActivity::onEnter() {
  RenderLock lock;
  UiListActivity::onEnter();
  rebuildRows();
}

void EpubReaderMenuActivity::onExit() {
  rememberSelection();
  UiListActivity::onExit();
}

EpubReaderMenuActivity::Category EpubReaderMenuActivity::categoryFor(const MenuAction action) {
  switch (action) {
    case MenuAction::SELECT_CHAPTER:
    case MenuAction::FOOTNOTES:
    case MenuAction::GO_TO_PERCENT:
    case MenuAction::BOOKMARKS:
      return Category::NAVIGATION;
    case MenuAction::TOGGLE_BOOKMARK:
    case MenuAction::SAVE_CLIPPING:
    case MenuAction::VIEW_CLIPPINGS:
    case MenuAction::DICTIONARY:
    case MenuAction::AUTO_PAGE_TURN:
      return Category::TOOLS;
    case MenuAction::TEXT_SETTINGS:
    case MenuAction::NIGHT_MODE:
    case MenuAction::FRONTLIGHT:
    case MenuAction::ROTATE_SCREEN:
      return Category::APPEARANCE;
    case MenuAction::BLUETOOTH:
    case MenuAction::SCREENSHOT:
    case MenuAction::DISPLAY_QR:
    case MenuAction::GO_HOME:
    case MenuAction::SYNC:
    case MenuAction::DELETE_CACHE:
      return Category::UTILITIES;
  }
  return Category::UTILITIES;
}

StrId EpubReaderMenuActivity::categoryLabel(const Category category) {
  static constexpr StrId labels[] = {StrId::STR_MENU_NAVIGATION, StrId::STR_MENU_TOOLS, StrId::STR_MENU_APPEARANCE,
                                     StrId::STR_MENU_UTILITIES};
  static_assert(std::size(labels) == CATEGORY_COUNT);
  return labels[static_cast<size_t>(category)];
}

void EpubReaderMenuActivity::rememberSelection() {
  if (nav.selected < 0 || nav.selected >= visibleRowCount) return;
  const auto& row = visibleRows[nav.selected];
  menuState.selectedCategory = row.category;
  menuState.categorySelected = row.menuIndex < 0;
  if (row.menuIndex >= 0) menuState.action = menuItems[row.menuIndex].action;
}

void EpubReaderMenuActivity::rebuildRows() {
  closeRouting();
  visibleRowCount = 0;
  int selected = 0;
  for (size_t c = 0; c < CATEGORY_COUNT; ++c) {
    const auto category = static_cast<Category>(c);
    const bool open = menuState.expanded && menuState.category == category;
    const int header = visibleRowCount++;
    visibleRows[header] = {category, -1};
    menuRowItems[header] = {};
    menuRowItems[header].label = I18N.get(categoryLabel(category));
    menuRowItems[header].bold = true;
    menuRowItems[header].value = open ? "-" : "+";
    menuRowItems[header].actionValue = header;
    if (menuState.selectedCategory == category) selected = header;
    if (!open) continue;
    for (size_t i = 0; i < menuItems.size(); ++i) {
      if (categoryFor(menuItems[i].action) != category) continue;
      const int row = visibleRowCount++;
      visibleRows[row] = {category, static_cast<int>(i)};
      menuRowItems[row] = {};
      menuRowItems[row].label = I18N.get(menuItems[i].labelId);
      menuRowItems[row].actionValue = row;
      if (!menuState.categorySelected && menuState.action == menuItems[i].action) selected = row;
    }
  }
  nav.reset(selected);
}

void EpubReaderMenuActivity::switchCategory(const int direction) {
  RenderLock lock;
  const int current = static_cast<int>(visibleRows[nav.selected].category);
  const int count = static_cast<int>(CATEGORY_COUNT);
  menuState.category = static_cast<Category>((current + direction + count) % count);
  menuState.selectedCategory = menuState.category;
  menuState.expanded = true;
  menuState.categorySelected = false;
  for (const auto& item : menuItems) {
    if (categoryFor(item.action) == menuState.category) {
      menuState.action = item.action;
      break;
    }
  }
  app.clearTapFlash();
  rebuildRows();
  requestUpdate();
}

void EpubReaderMenuActivity::navigateButtons() {
  if (mappedInput.hasRemoteClick()) {
    UiListActivity::navigateButtons();
    return;
  }
  using Button = MappedInputManager::Button;
  const bool swapped = mappedInput.isNavDirectionSwapped();
  buttonNavigator.onPress({swapped ? Button::Right : Button::Left}, [this] { switchCategory(-1); });
  buttonNavigator.onPress({swapped ? Button::Left : Button::Right}, [this] { switchCategory(1); });
  buttonNavigator.onPressAndContinuous({swapped ? Button::Down : Button::Up}, [this] {
    moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, listCount()));
  });
  buttonNavigator.onPressAndContinuous({swapped ? Button::Up : Button::Down}, [this] {
    moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, listCount()));
  });
}

void EpubReaderMenuActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_PREVIOUS_GROUP), tr(STR_NEXT_GROUP));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, const bool hasFootnotes,
                                            const bool hasBookmarks, const bool hasClippings) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});

  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (hasClippings) {
    items.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  if (SETTINGS.dictionaryName[0] != '\0') {
    items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  }
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});

  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
  items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present() && !slideSwitchOwnsLight()) {
    items.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});

  if (bleturner::status().compiledIn) {
    items.push_back({MenuAction::BLUETOOTH, StrId::STR_BT_PAGE_TURNER});
  }
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
}

void EpubReaderMenuActivity::toggleBluetooth() {
  SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
  if (!SETTINGS.ble.enabled) bleturner::switchOff();
  SETTINGS.saveToFile();
  // Enabling waits for the main-loop lifecycle's reader heap gates, then says in the status bar
  // whether a remote linked.
  if (SETTINGS.ble.enabled) bleturner::requestConnect();
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
  setResult(std::move(result));
  finish();
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  if (index < 0 || index >= visibleRowCount) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const auto row = visibleRows[index];
  if (row.menuIndex < 0) {
    RenderLock lock;
    const bool wasOpen = menuState.expanded && menuState.category == row.category;
    menuState.category = row.category;
    menuState.selectedCategory = row.category;
    menuState.expanded = !wasOpen;
    menuState.categorySelected = true;
    rebuildRows();
    requestUpdate();
    return;
  }

  const auto selectedAction = menuItems[row.menuIndex].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       nav.followOnBuild = true;
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
    optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                     static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                       selectedPageTurnOption = idx;
                       requestUpdate();
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::NIGHT_MODE) {
    SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::BLUETOOTH) {
    toggleBluetooth();
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FRONTLIGHT) {
    const bool lightOn = !Frontlight.isOn();
    Frontlight.setOn(lightOn);
    SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
  finish();
}

bool EpubReaderMenuActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool EpubReaderMenuActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    closeCancelled();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  // Progress summary where the old sub-header band sat.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), progressLine.c_str(), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < visibleRowCount; ++i) {
    if (visibleRows[i].menuIndex < 0) continue;
    const auto action = menuItems[visibleRows[i].menuIndex].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      menuRowItems[i].value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      menuRowItems[i].value = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::NIGHT_MODE) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.screenInverted);
    } else if (action == MenuAction::BLUETOOTH) {
      GUI.setCheckboxRow(menuRowItems[i], SETTINGS.ble.enabled);
    } else if (action == MenuAction::FRONTLIGHT) {
      GUI.setCheckboxRow(menuRowItems[i], Frontlight.isOn());
    }
  }

  fui::ListProps props;
  props.items = menuRowItems;
  props.count = static_cast<uint16_t>(visibleRowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());
}

void EpubReaderMenuActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  UiListActivity::render(std::move(lock));
}
