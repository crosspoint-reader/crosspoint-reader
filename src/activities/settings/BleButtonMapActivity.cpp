#include "BleButtonMapActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "BleInput.h"
#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
  Activity::onEnter();
  step = Step::WaitForKey;
  capturedKind = 0xFF;
  functionIndex = 0;
  // Start every mapping session from a clean slate: the user re-maps each remote
  // button once, so a button can't be left bound to a stale action and there's no
  // separate "clear mappings" step to remember.
  std::fill(std::begin(SETTINGS.bleKeyMap), std::end(SETTINGS.bleKeyMap), CrossPointSettings::BleKeyMapEntry{});
  SETTINGS.saveToFile();
  mappedInput.setBleCaptureMode(true);
  requestUpdate();
}

void BleButtonMapActivity::onExit() {
  mappedInput.setBleCaptureMode(false);
  Activity::onExit();
}

bool BleButtonMapActivity::assignCapturedKey(MappedInputManager::Button button) {
  const uint8_t btn = static_cast<uint8_t>(button);
  auto& map = SETTINGS.bleKeyMap;
  using Entry = CrossPointSettings::BleKeyMapEntry;
  const uint8_t kind = capturedKind;
  const uint8_t value = capturedValue;

  // One key per action: drop any other key currently bound to this action so the same
  // action can't be triggered by two different remote buttons.
  std::replace_if(
      std::begin(map), std::end(map),
      [&](const Entry& e) { return e.button == btn && !(e.keyKind == kind && e.keyValue == value); }, Entry{});

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

void BleButtonMapActivity::loop() {
  // Front Back button exits the mapping screen at any step.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (step == Step::WaitForKey) {
    uint8_t kind = 0xFF;
    uint8_t value = 0;
    if (mappedInput.takeCapturedBleKey(kind, value)) {
      capturedKind = kind;
      capturedValue = value;
      functionIndex = 0;
      step = Step::SelectFunction;
      requestUpdate();
    }
    return;
  }

  // Step::SelectFunction — pick a logical function for the captured key.
  buttonNavigator.onNext([this] {
    functionIndex = ButtonNavigator::nextIndex(functionIndex, kFunctionCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    functionIndex = ButtonNavigator::previousIndex(functionIndex, kFunctionCount);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    assignCapturedKey(kFunctions[functionIndex].button);
    // Back to capturing so the user can map (or re-map) the next remote button.
    step = Step::WaitForKey;
    capturedKind = 0xFF;
    requestUpdate();
  }
}

void BleButtonMapActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_MAP_BUTTONS));

  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (step == Step::WaitForKey) {
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      tr(STR_BT_PRESS_REMOTE));
    // Show the current mappings so the user sees progress.
    int row = 0;
    for (const auto& e : SETTINGS.bleKeyMap) {
      if (e.button == 0xFF) continue;
      char keyName[24];
      bleinput::describeKey(e.keyKind, e.keyValue, keyName, sizeof(keyName));
      const char* fnName = "";
      for (uint8_t i = 0; i < kFunctionCount; i++) {
        if (static_cast<uint8_t>(kFunctions[i].button) == e.button) {
          fnName = I18N.get(kFunctions[i].label);
          break;
        }
      }
      char line[64];
      snprintf(line, sizeof(line), "%s  ->  %s", keyName, fnName);
      GUI.drawHelpText(renderer, Rect{0, topOffset + row * 22, pageWidth, 20}, line);
      row++;
    }
  } else {
    char captured[24];
    bleinput::describeKey(capturedKind, capturedValue, captured, sizeof(captured));
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      captured);
    GUI.drawList(
        renderer, Rect{0, topOffset, pageWidth, contentHeight}, kFunctionCount, functionIndex,
        [this](int i) { return std::string(I18N.get(kFunctions[i].label)); }, nullptr, nullptr, nullptr, false);
  }

  const char* confirm = step == Step::WaitForKey ? "" : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
