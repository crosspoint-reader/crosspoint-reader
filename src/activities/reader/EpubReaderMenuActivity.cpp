#include "EpubReaderMenuActivity.h"

#include <BlePageTurner.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "util/SlideSwitch.h"

namespace fui = freeink::ui;

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool hasClippings, const SdCardFontRegistry* fontRegistry)
    : UiListActivity("EpubReaderMenu", renderer, mappedInput),
      title(title),
      pendingOrientation(currentOrientation),
      fontRegistry(fontRegistry),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(menuItems, hasFootnotes, hasBookmarks, hasClippings);
  buildMenuRowItems();
  // Show the size the reader renders: the stored size snapped into the family's set.
  initialFontFamily = readerFontFamilyIndex(fontRegistry, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  initialFontPointSize = snapToNearestPointSize(
      readerFontPointSizes(fontRegistry, readerFontFamilySdName(fontRegistry, initialFontFamily)),
      SETTINGS.fontPointSize);
  setPendingFont(initialFontFamily, initialFontPointSize);
}

// Populates menuRowItems's labels/actionValue from menuItems. Called once
// here since menuItems (and thus which rows exist) never changes after
// construction; buildScreen() only touches the two rows with a live value.
void EpubReaderMenuActivity::buildMenuRowItems() {
  for (size_t i = 0; i < menuItems.size() && i < MAX_MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i].labelId);
    item.actionValue = static_cast<int16_t>(i);
    menuRowItems[i] = item;
  }
}

void EpubReaderMenuActivity::buildMenuItems(std::vector<MenuItem>& items, const bool hasFootnotes,
                                            const bool hasBookmarks, const bool hasClippings) {
  items.clear();
  items.reserve(MAX_MENU_ITEMS);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::FONT_FAMILY, StrId::STR_FONT_FAMILY});
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE});
  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (hasClippings) {
    items.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present() && !slideSwitchOwnsLight()) {
    items.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  if (bleturner::status().compiledIn) {
    items.push_back({MenuAction::BLUETOOTH, StrId::STR_BT_PAGE_TURNER});
  }
  if (SETTINGS.dictionaryName[0] != '\0') {
    items.push_back({MenuAction::DICTIONARY, StrId::STR_LOOKUP});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
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

MenuResult EpubReaderMenuActivity::makeResult(const int action) const {
  MenuResult result{action, pendingOrientation, selectedPageTurnOption};
  const bool familyChanged = pendingFontFamily != initialFontFamily;
  // A size-only change leaves the stored family alone, even one that is not
  // installed right now.
  if (familyChanged) result.fontFamily = static_cast<int16_t>(pendingFontFamily);
  if (familyChanged || pendingFontPointSize != initialFontPointSize) result.fontPointSize = pendingFontPointSize;
  return result;
}

void EpubReaderMenuActivity::closeCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = makeResult(-1);
  setResult(std::move(result));
  finish();
}

const char* EpubReaderMenuActivity::fontFamilyLabel(const int family) const {
  const char* sdName = readerFontFamilySdName(fontRegistry, family);
  return sdName && sdName[0] != '\0' ? sdName : I18N.get(StrId::STR_NOTO_SANS);
}

void EpubReaderMenuActivity::setPendingFont(const int family, const uint8_t pointSize) {
  pendingFontFamily = family;
  pendingFontPointSize = pointSize;
  snprintf(fontSizeValue, sizeof(fontSizeValue), "%u pt", pointSize);
}

void EpubReaderMenuActivity::showFontFamilyPicker() {
  const int count = readerFontFamilyCount(fontRegistry);
  // Transient: show() copies the labels into the popup.
  std::vector<const char*> labels;
  labels.reserve(count);
  for (int i = 0; i < count; i++) labels.push_back(fontFamilyLabel(i));
  optionPopup.show(I18N.get(StrId::STR_FONT_FAMILY), labels.data(), count, pendingFontFamily, [this](int idx) {
    // The new family may not ship the current size; take its nearest one, as
    // SdCardFontSystem::ensureLoaded() would.
    const auto sizes = readerFontPointSizes(fontRegistry, readerFontFamilySdName(fontRegistry, idx));
    setPendingFont(idx, snapToNearestPointSize(sizes, pendingFontPointSize));
    requestUpdate();
  });
}

void EpubReaderMenuActivity::showFontSizePicker() {
  // The point sizes the picked family actually ships.
  auto sizes = readerFontPointSizes(fontRegistry, readerFontFamilySdName(fontRegistry, pendingFontFamily));
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  int current = 0;
  for (size_t i = 0; i < sizes.size(); i++) {
    labels.push_back(std::to_string(sizes[i]) + " pt");
    if (sizes[i] == pendingFontPointSize) current = static_cast<int>(i);
  }
  optionPopup.show(StrId::STR_FONT_SIZE, labels, current, [this, sizes = std::move(sizes)](int idx) {
    if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
    setPendingFont(pendingFontFamily, sizes[idx]);
    requestUpdate();
  });
}

bool EpubReaderMenuActivity::handleHomeGesture() {
  closeCancelled();
  return true;
}

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // The activated row leaves this screen (popup or finish); a lingering flash
  // would gray an unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;

  const auto selectedAction = menuItems[index].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately. Only the renderer turns;
                       // SETTINGS.orientation stays unchanged so the reader's
                       // result handler still detects the change and reflows.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());  // hit rects follow the new frame
                       requestUpdate(true);
                     });
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FONT_FAMILY || selectedAction == MenuAction::FONT_SIZE) {
    if (selectedAction == MenuAction::FONT_FAMILY) {
      showFontFamilyPicker();
    } else {
      showFontSizePicker();
    }
    bool fits;
    {
      // Measuring can load SD fallback glyphs, which the render task shares.
      RenderLock lock;
      fits = optionPopup.fitsScreen(renderer);
    }
    if (!fits) {
      // The popup cannot scroll; the reader opens the matching Text Settings
      // tab, whose list does.
      optionPopup.dismiss();
      setResult(makeResult(static_cast<int>(selectedAction)));
      finish();
      return;
    }
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

  setResult(makeResult(static_cast<int>(selectedAction)));
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

  // menuRowItems's labels/actionValue were set once in the constructor (see
  // buildMenuRowItems()); only rows with live values need refreshing here.
  for (size_t i = 0; i < menuItems.size(); i++) {
    const auto action = menuItems[i].action;
    if (action == MenuAction::ROTATE_SCREEN) {
      menuRowItems[i].value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (action == MenuAction::AUTO_PAGE_TURN) {
      menuRowItems[i].value = pageTurnLabels[selectedPageTurnOption];
    } else if (action == MenuAction::FONT_FAMILY) {
      menuRowItems[i].value = fontFamilyLabel(pendingFontFamily);
    } else if (action == MenuAction::FONT_SIZE) {
      menuRowItems[i].value = fontSizeValue;
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
  props.count = static_cast<uint16_t>(menuItems.size());
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
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false), title.c_str());
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  drawChrome();

  renderUi();

  drawFooter();
  renderer.displayBuffer();
}
