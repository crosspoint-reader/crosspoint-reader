#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// UI steps correspond to logical roles in order: Back, Confirm, Left, Right.
constexpr uint8_t kRoleCount = 4;
// Marker used when a role has not been assigned yet.
constexpr uint8_t kUnassigned = 0xFF;
// Duration to show temporary error text when reassigning a button.
constexpr unsigned long kErrorDisplayMs = 1500;
}  // namespace

void ButtonRemapActivity::onEnter() {
  Activity::onEnter();

  // Start with all roles unassigned to avoid duplicate blocking.
  currentStep = 0;
  tempMapping[0] = kUnassigned;
  tempMapping[1] = kUnassigned;
  tempMapping[2] = kUnassigned;
  tempMapping[3] = kUnassigned;
  errorMessage.clear();
  errorUntil = 0;
  requestUpdate();
}

void ButtonRemapActivity::onExit() { Activity::onExit(); }

void ButtonRemapActivity::loop() {
  // Clear any temporary warning after its timeout.
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return;
  }

  // Side buttons:
  // - Up: reset mapping to defaults and exit.
  // - Down: cancel without saving.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Persist default mapping immediately so the user can recover quickly.
    SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
    SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
    SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
    SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    SETTINGS.saveToFile();
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    // Exit without changing settings.
    onBack();
    return;
  }

  {
    // Wait for the UI to refresh before accepting another assignment.
    // This avoids rapid double-presses that can advance the step without a visible redraw.
    requestUpdateAndWait();

    // Wait for a front button press to assign to the current role.
    const int pressedButton = mappedInput.getPressedFrontButton();
    if (pressedButton < 0) {
      return;
    }

    // Update temporary mapping and advance the remap step.
    // Only accept the press if this hardware button isn't already assigned elsewhere.
    if (!validateUnassigned(static_cast<uint8_t>(pressedButton))) {
      requestUpdate();
      return;
    }
    tempMapping[currentStep] = static_cast<uint8_t>(pressedButton);
    currentStep++;

    if (currentStep >= kRoleCount) {
      // All roles assigned; save to settings and exit.
      applyTempMapping();
      SETTINGS.saveToFile();
      onBack();
      return;
    }

    requestUpdate();
  }
}

void ButtonRemapActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto labelForHardware = [&](uint8_t hardwareIndex) -> const char* {
    for (uint8_t i = 0; i < kRoleCount; i++) {
      if (tempMapping[i] == hardwareIndex) {
        return getRoleName(i);
      }
    }
    return "-";
  };

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Remap Front Buttons", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 40, "Press a front button for each role");

  for (uint8_t i = 0; i < kRoleCount; i++) {
    const int y = 70 + i * 30;
    const bool isSelected = (i == currentStep);

    // Highlight the role that is currently being assigned.
    if (isSelected) {
      renderer.fillRect(0, y - 2, pageWidth - 1, 30);
    }

    const char* roleName = getRoleName(i);
    renderer.drawText(UI_10_FONT_ID, 20, y, roleName, !isSelected);

    // Show currently assigned hardware button (or unassigned).
    const char* assigned = (tempMapping[i] == kUnassigned) ? "Unassigned" : getHardwareName(tempMapping[i]);
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, assigned);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, y, assigned, !isSelected);
  }

  // Temporary warning banner for duplicates.
  if (!errorMessage.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 210, errorMessage.c_str(), true);
  }

  // Provide side button actions at the bottom of the screen (split across two lines).
  renderer.drawCenteredText(SMALL_FONT_ID, 250, "Side button Up: Reset to default layout", true);
  renderer.drawCenteredText(SMALL_FONT_ID, 280, "Side button Down: Cancel remapping", true);

  // Live preview of logical labels under front buttons.
  // This mirrors the on-device front button order: Back, Confirm, Left, Right.
  GUI.drawButtonHints(renderer, labelForHardware(CrossPointSettings::FRONT_HW_BACK),
                      labelForHardware(CrossPointSettings::FRONT_HW_CONFIRM),
                      labelForHardware(CrossPointSettings::FRONT_HW_LEFT),
                      labelForHardware(CrossPointSettings::FRONT_HW_RIGHT));
  renderer.displayBuffer();
}

void ButtonRemapActivity::applyTempMapping() {
  // Commit temporary mapping into settings (logical role -> hardware).
  SETTINGS.frontButtonBack = tempMapping[0];
  SETTINGS.frontButtonConfirm = tempMapping[1];
  SETTINGS.frontButtonLeft = tempMapping[2];
  SETTINGS.frontButtonRight = tempMapping[3];
}

bool ButtonRemapActivity::validateUnassigned(const uint8_t pressedButton) {
  // Block reusing a hardware button already assigned to another role.
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == pressedButton && i != currentStep) {
      errorMessage = "Already assigned";
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* ButtonRemapActivity::getRoleName(const uint8_t roleIndex) const {
  switch (roleIndex) {
    case 0:
      return "Back";
    case 1:
      return "Confirm";
    case 2:
      return "Left";
    case 3:
    default:
      return "Right";
  }
}

const char* ButtonRemapActivity::getHardwareName(const uint8_t buttonIndex) const {
  switch (buttonIndex) {
    case CrossPointSettings::FRONT_HW_BACK:
      return "Back (1st button)";
    case CrossPointSettings::FRONT_HW_CONFIRM:
      return "Confirm (2nd button)";
    case CrossPointSettings::FRONT_HW_LEFT:
      return "Left (3rd button)";
    case CrossPointSettings::FRONT_HW_RIGHT:
      return "Right (4th button)";
    default:
      return "Unknown";
  }
}
