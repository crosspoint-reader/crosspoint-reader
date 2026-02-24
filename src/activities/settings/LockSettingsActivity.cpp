#include "LockSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kDotRadius = 8;
constexpr int kDotSpacing = 28;
constexpr int kMaxCodeLength = 6;
constexpr int kMinCodeLength = 3;
constexpr int kMenuItems = 3;  // Change Code, Disable Lock, Back
}  // namespace

void LockSettingsActivity::onEnter() {
  Activity::onEnter();
  messageUntil = 0;
  messageText = nullptr;

  if (SETTINGS.lockEnabled && SETTINGS.lockSequenceLength >= kMinCodeLength) {
    mode = Mode::MENU;
    menuSelection = 0;
  } else {
    mode = Mode::SETUP;
    newCodeLength = 0;
  }
  requestUpdate();
}

void LockSettingsActivity::onExit() { Activity::onExit(); }

void LockSettingsActivity::showMessage(const char* text, unsigned long durationMs) {
  messageText = text;
  messageUntil = millis() + durationMs;
  requestUpdate();
}

int LockSettingsActivity::getPressedButton() {
  int frontBtn = mappedInput.getPressedFrontButton();
  if (frontBtn >= 0 && frontBtn <= 3) return frontBtn;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) return 4;
  return -1;
}

void LockSettingsActivity::loop() {
  // Clear temporary messages
  if (messageUntil > 0 && millis() > messageUntil) {
    messageText = nullptr;
    messageUntil = 0;
    requestUpdate();
    return;
  }

  if (messageUntil > 0) return;

  switch (mode) {
    case Mode::MENU: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        handleMenuConfirm();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        menuSelection = (menuSelection + kMenuItems - 1) % kMenuItems;
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        menuSelection = (menuSelection + 1) % kMenuItems;
        requestUpdate();
      }
      break;
    }
    case Mode::SETUP: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        if (SETTINGS.lockEnabled) {
          mode = Mode::MENU;
          menuSelection = 0;
          requestUpdate();
        } else {
          finish();
        }
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down) && newCodeLength >= kMinCodeLength) {
        mode = Mode::CONFIRM;
        confirmCodeLength = 0;
        requestUpdate();
        return;
      }
      int pressed = getPressedButton();
      if (pressed >= 0) handleCodeInput(pressed);
      break;
    }
    case Mode::CONFIRM: {
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        mode = Mode::SETUP;
        newCodeLength = 0;
        requestUpdate();
        return;
      }
      int pressed = getPressedButton();
      if (pressed >= 0) handleConfirmInput(pressed);
      break;
    }
  }
}

void LockSettingsActivity::handleMenuConfirm() {
  switch (menuSelection) {
    case 0:  // Change Code
      mode = Mode::SETUP;
      newCodeLength = 0;
      requestUpdate();
      break;
    case 1:  // Disable Lock
      SETTINGS.lockEnabled = 0;
      SETTINGS.lockSequenceLength = 0;
      memset(SETTINGS.lockSequence, 0, sizeof(SETTINGS.lockSequence));
      SETTINGS.saveToFile();
      finish();
      break;
    case 2:  // Back
      finish();
      break;
  }
}

void LockSettingsActivity::handleCodeInput(int pressed) {
  if (newCodeLength < kMaxCodeLength) {
    newCode[newCodeLength] = static_cast<uint8_t>(pressed);
    newCodeLength++;
    requestUpdate();

    // Auto-finish at max length
    if (newCodeLength == kMaxCodeLength) {
      mode = Mode::CONFIRM;
      confirmCodeLength = 0;
      requestUpdate();
    }
  }
}

void LockSettingsActivity::handleConfirmInput(int pressed) {
  confirmCode[confirmCodeLength] = static_cast<uint8_t>(pressed);
  confirmCodeLength++;
  requestUpdate();

  if (confirmCodeLength == newCodeLength) {
    if (memcmp(newCode, confirmCode, newCodeLength) == 0) {
      // Match — save
      memcpy(SETTINGS.lockSequence, newCode, newCodeLength);
      SETTINGS.lockSequenceLength = newCodeLength;
      SETTINGS.lockEnabled = 1;
      SETTINGS.saveToFile();
      showMessage(tr(STR_LOCK_CODE_SET));
      mode = Mode::MENU;
      menuSelection = 0;
    } else {
      showMessage(tr(STR_CODES_DONT_MATCH));
      mode = Mode::SETUP;
      newCodeLength = 0;
    }
  }
}

void LockSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LOCK_SCREEN));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (mode == Mode::MENU) {
    const StrId menuNames[kMenuItems] = {StrId::STR_CHANGE_CODE, StrId::STR_DISABLE_LOCK, StrId::STR_BACK};
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, kMenuItems, menuSelection,
        [&menuNames](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr, nullptr, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // SETUP or CONFIRM mode — show instruction + dots
    const bool isConfirm = (mode == Mode::CONFIRM);
    const char* instruction = isConfirm ? tr(STR_CONFIRM_CODE) : tr(STR_ENTER_NEW_CODE);
    const uint8_t currentLength = isConfirm ? confirmCodeLength : newCodeLength;
    const int maxDots = isConfirm ? newCodeLength : kMaxCodeLength;

    // Center content between header and button hints
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int centerY = (contentTop + contentBottom) / 2;

    // Draw instruction text
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, instruction, true, EpdFontFamily::BOLD);

    // Draw dots
    const int dotY = centerY + 20;
    const int totalWidth = maxDots * kDotRadius * 2 + (maxDots - 1) * (kDotSpacing - kDotRadius * 2);
    const int startX = (pageWidth - totalWidth) / 2;

    for (int i = 0; i < maxDots; i++) {
      const int cx = startX + i * kDotSpacing + kDotRadius;
      const int cy = dotY;

      if (i < currentLength) {
        renderer.fillRoundedRect(cx - kDotRadius, cy - kDotRadius, kDotRadius * 2, kDotRadius * 2, kDotRadius,
                                 Color::Black);
      } else {
        renderer.drawRoundedRect(cx - kDotRadius, cy - kDotRadius, kDotRadius * 2, kDotRadius * 2, 1, kDotRadius,
                                 true);
      }
    }

    // Show temporary message (e.g. "Codes don't match")
    if (messageText) {
      renderer.drawCenteredText(UI_10_FONT_ID, dotY + 40, messageText, true, EpdFontFamily::BOLD);
    }

    const char* frontLabels[] = {ButtonShape::CIRCLE, ButtonShape::SQUARE, ButtonShape::TRIANGLE, ButtonShape::CROSS};
    frontLabels[SETTINGS.frontButtonBack] = tr(STR_BACK);
    GUI.drawButtonHints(renderer, frontLabels[0], frontLabels[1], frontLabels[2], frontLabels[3]);
    const bool canFinish = !isConfirm && currentLength >= kMinCodeLength && currentLength < kMaxCodeLength;
    GUI.drawSideButtonHints(renderer, ButtonShape::DIAMOND, canFinish ? "OK" : "");
  }

  renderer.displayBuffer();
}
