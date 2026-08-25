#include "BleButtonMapActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "BleInput.h"
#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

// Logical functions offered for binding. Page navigation + confirm cover Free2 /
// Free3; the directions are included so a remote can also drive menu navigation.
const BleButtonMapActivity::Fn BleButtonMapActivity::kFunctions[] = {
    {MappedInputManager::Button::PageForward, StrId::STR_BT_PAGE_FORWARD},
    {MappedInputManager::Button::PageBack, StrId::STR_BT_PAGE_BACK},
    {MappedInputManager::Button::Confirm, StrId::STR_CONFIRM},
    {MappedInputManager::Button::Back, StrId::STR_BACK},
    {MappedInputManager::Button::Up, StrId::STR_DIR_UP},
    {MappedInputManager::Button::Down, StrId::STR_DIR_DOWN},
    {MappedInputManager::Button::Left, StrId::STR_DIR_LEFT},
    {MappedInputManager::Button::Right, StrId::STR_DIR_RIGHT},
};
const uint8_t BleButtonMapActivity::kFunctionCount = static_cast<uint8_t>(sizeof(kFunctions) / sizeof(kFunctions[0]));

void BleButtonMapActivity::onEnter() {
  captureTarget = -1;
  UiListActivity::onEnter();
  // The remote must be connected for capture to see its keys; make sure the
  // stack is up even if the lifecycle's next tick hasn't run yet.
  if (SETTINGS.bluetoothEnabled && !BleHid.isRunning()) bleinput::ensureStarted();
}

void BleButtonMapActivity::onExit() {
  mappedInput.setBleCaptureMode(false);
  Activity::onExit();
}

int BleButtonMapActivity::listCount() const {
  // The list is inert while a capture is armed — the remote is the input device.
  return captureTarget >= 0 ? 0 : kFunctionCount;
}

void BleButtonMapActivity::rebuildRows() {
  for (uint8_t i = 0; i < kFunctionCount && i < 16; i++) {
    rowItems[i] = fui::ListItem{};
    rowItems[i].label = I18N.get(kFunctions[i].label);
    rowItems[i].actionValue = static_cast<int16_t>(i);
    // Show the remote key currently bound to this function, if any.
    const uint8_t btn = static_cast<uint8_t>(kFunctions[i].button);
    const auto* entry = std::find_if(
        std::begin(SETTINGS.bleKeyMap), std::end(SETTINGS.bleKeyMap),
        [btn](const CrossPointSettings::BleKeyMapEntry& e) { return e.button == btn && e.keyKind != 0xFF; });
    if (entry != std::end(SETTINGS.bleKeyMap)) {
      bleinput::describeKey(entry->keyKind, entry->keyValue, valueBuf[i], sizeof(valueBuf[i]));
    } else {
      snprintf(valueBuf[i], sizeof(valueBuf[i]), "%s", tr(STR_BT_NOT_MAPPED));
    }
    rowItems[i].value = valueBuf[i];
  }
}

void BleButtonMapActivity::beginCapture(const int index) {
  captureTarget = index;
  mappedInput.setBleCaptureMode(true);
  requestUpdate();
}

void BleButtonMapActivity::endCapture() {
  mappedInput.setBleCaptureMode(false);
  captureTarget = -1;
  requestUpdate();
}

bool BleButtonMapActivity::assignKey(const MappedInputManager::Button button, const uint8_t kind, const uint8_t value) {
  const uint8_t btn = static_cast<uint8_t>(button);
  // Mutated via std::replace_if below and through `slot`; cppcheck's CI parse
  // (no include paths) can't see the writes and suggests const.
  // cppcheck-suppress constVariableReference
  auto& map = SETTINGS.bleKeyMap;
  using Entry = CrossPointSettings::BleKeyMapEntry;

  // One key per action: drop any other key currently bound to this action so the same
  // action can't be triggered by two different remote buttons.
  std::replace_if(
      std::begin(map), std::end(map),
      [&](const Entry& e) { return e.button == btn && !(e.keyKind == kind && e.keyValue == value); }, Entry{});
  // One action per key: a key re-bound here must stop triggering its old action.
  std::replace_if(
      std::begin(map), std::end(map),
      [&](const Entry& e) { return e.keyKind == kind && e.keyValue == value && e.button != btn; }, Entry{});

  // Reuse the slot already bound to this key, else the first free slot.
  auto* slot = std::find_if(std::begin(map), std::end(map), [&](const Entry& e) {
    return e.button != 0xFF && e.keyKind == kind && e.keyValue == value;
  });
  if (slot == std::end(map)) {
    slot = std::find_if(std::begin(map), std::end(map),
                        [](const Entry& e) { return e.button == 0xFF || e.keyKind == 0xFF; });
  }
  if (slot == std::end(map)) return false;  // table full

  slot->keyKind = kind;
  slot->keyValue = value;
  slot->button = btn;
  SETTINGS.saveToFile();
  return true;
}

void BleButtonMapActivity::activateIndex(const int index) {
  if (index < 0 || index >= kFunctionCount) return;
  app.clearTapFlash();
  beginCapture(index);
}

bool BleButtonMapActivity::handleCustomInput() {
  if (captureTarget < 0) return false;

  // A capture is armed: the next remote key binds to the chosen function.
  uint8_t kind = 0xFF;
  uint8_t value = 0;
  if (mappedInput.takeCapturedBleKey(kind, value)) {
    if (!assignKey(kFunctions[captureTarget].button, kind, value)) {
      LOG_ERR("BLEUI", "key map full; cannot bind kind=%u value=%u", kind, value);
    }
    endCapture();
    return true;
  }

  // Back (button or gesture) cancels the capture instead of leaving the screen.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    endCapture();
    return true;
  }
  return true;  // consume everything else while armed
}

void BleButtonMapActivity::onBackButton() { finish(); }

void BleButtonMapActivity::drawChrome() {
  UiListActivity::drawChrome();
  // Capture prompt in the sub-header slot while armed.
  if (captureTarget >= 0) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawSubHeader(
        renderer, Rect{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight},
        tr(STR_BT_PRESS_REMOTE));
  }
}

void BleButtonMapActivity::buildScreen(UiScreen& screen) {
  rebuildRows();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band (plus the capture sub-header).
  const int16_t top = static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight +
                                           (captureTarget >= 0 ? metrics.tabBarHeight : 0));
  screen.setContentMargin(fui::Insets{top, static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (captureTarget >= 0) {
    // The armed function, centered, as the only content: the next input comes
    // from the remote, not this screen.
    screen.centeredText(I18N.get(kFunctions[captureTarget].label));
    return;
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(kFunctionCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
