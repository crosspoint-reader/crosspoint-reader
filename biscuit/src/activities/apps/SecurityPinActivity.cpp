#include "SecurityPinActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "MappedInputManager.h"
#include "QuickWipeActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DuressManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SecurityPinActivity::hashPin(const char* pin, uint8_t* out) {
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(pin), strlen(pin), out, 0);
}

bool SecurityPinActivity::checkPin(const char* pin, const uint8_t* hash) const {
  uint8_t computed[32];
  hashPin(pin, computed);
  volatile uint8_t diff = 0;
  for (int i = 0; i < 32; i++) {
    diff |= computed[i] ^ hash[i];
  }
  // Zero the computed hash before returning
  memset(computed, 0, sizeof(computed));
  return diff == 0;
}

void SecurityPinActivity::clearPinBuffer() {
  memset(pinBuffer, 0, sizeof(pinBuffer));
  pinPos = 0;
}

void SecurityPinActivity::showMessage(const char* msg, unsigned long durationMs) {
  snprintf(msgBuf, sizeof(msgBuf), "%s", msg);
  msgUntilMs = millis() + durationMs;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void SecurityPinActivity::loadConfig() {
  auto file = Storage.open(PIN_PATH);
  if (!file) return;

  uint8_t buf[65];
  int read = file.read(buf, 65);
  file.close();

  if (read < 65) return;

  memcpy(storedPinHash, buf, 32);
  memcpy(storedDuressHash, buf + 32, 32);
  flags = buf[64];
}

void SecurityPinActivity::saveConfig() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(PIN_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return;

  file.write(storedPinHash, 32);
  file.write(storedDuressHash, 32);
  file.write(&flags, 1);
  file.close();
}

bool SecurityPinActivity::isPinEnabled() {
  auto file = Storage.open(PIN_PATH);
  if (!file) return false;

  uint8_t buf[65];
  int read = file.read(buf, 65);
  file.close();

  if (read < 65) return false;
  return (buf[64] & 0x01) != 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SecurityPinActivity::onEnter() {
  Activity::onEnter();

  loadConfig();
  clearPinBuffer();
  memset(msgBuf, 0, sizeof(msgBuf));
  msgUntilMs = 0;
  failCount = 0;

  // If no PIN is configured, go straight to settings so the user can set one
  if (!(flags & 0x01)) {
    state = SETTINGS_MENU;
    menuIndex = 0;
  } else {
    state = ENTER_PIN;
    pinLength = 4;  // default; actual length is unknown until correct PIN entered
  }

  requestUpdate();
}

void SecurityPinActivity::onExit() {
  memset(pinBuffer, 0, sizeof(pinBuffer));
  memset(newPin, 0, sizeof(newPin));
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void SecurityPinActivity::loop() {
  // Clear timed message
  if (msgUntilMs && millis() >= msgUntilMs) {
    msgUntilMs = 0;
    msgBuf[0] = '\0';
    requestUpdate();
  }

  switch (state) {
    case ENTER_PIN:    handlePinEntry();    break;
    case SET_PIN:
    case SET_DURESS_PIN: handleSetPin();   break;
    case SETTINGS_MENU: handleSettingsMenu(); break;
  }
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

void SecurityPinActivity::handlePinEntry() {
  // Back button intentionally blocked — user must authenticate

  // Left/Right: cycle active digit value 0-9
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    char cur = pinBuffer[pinPos];
    if (cur == 0) cur = '9';
    else if (cur == '0') cur = '9';
    else cur--;
    pinBuffer[pinPos] = cur;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    char cur = pinBuffer[pinPos];
    if (cur == 0) cur = '0';
    else if (cur == '9') cur = '0';
    else cur++;
    pinBuffer[pinPos] = cur;
    requestUpdate();
    return;
  }

  // Up: move position left (previous digit)
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (pinPos > 0) { pinPos--; requestUpdate(); }
    return;
  }
  // Down: move position right (next digit)
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (pinPos < pinLength - 1) { pinPos++; requestUpdate(); }
    return;
  }

  // Confirm: submit PIN
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Ensure all digits have been explicitly set (non-zero char means set)
    for (int i = 0; i < pinLength; i++) {
      if (pinBuffer[i] == 0) pinBuffer[i] = '0';
    }
    pinBuffer[pinLength] = '\0';

    // Check main PIN
    if ((flags & 0x01) && checkPin(pinBuffer, storedPinHash)) {
      DURESS.deactivate();
      finish();
      return;
    }
    // Check duress PIN
    if ((flags & 0x02) && checkPin(pinBuffer, storedDuressHash)) {
      DURESS.activate();
      finish();
      return;
    }

    // Wrong PIN
    failCount++;
    if (failCount >= MAX_FAILS && (flags & 0x04)) {
      QuickWipeActivity::performWipe();
      finish();
      return;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "Wrong PIN (%d/%d)", failCount, MAX_FAILS);
    showMessage(buf);
    clearPinBuffer();
  }
}

void SecurityPinActivity::handleSetPin() {
  // Back cancels back to SETTINGS_MENU
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    state = SETTINGS_MENU;
    settingFirstEntry = true;
    clearPinBuffer();
    memset(newPin, 0, sizeof(newPin));
    requestUpdate();
    return;
  }

  // Left/Right: cycle active digit
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    char cur = pinBuffer[pinPos];
    if (cur == 0) cur = '9';
    else if (cur == '0') cur = '9';
    else cur--;
    pinBuffer[pinPos] = cur;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    char cur = pinBuffer[pinPos];
    if (cur == 0) cur = '0';
    else if (cur == '9') cur = '0';
    else cur++;
    pinBuffer[pinPos] = cur;
    requestUpdate();
    return;
  }

  // Up/Down: move between digit positions
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (pinPos > 0) { pinPos--; requestUpdate(); }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (pinPos < pinLength - 1) { pinPos++; requestUpdate(); }
    return;
  }

  // PageForward: change PIN length (4, 5, or 6 digits)
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    pinLength = (pinLength < 6) ? pinLength + 1 : 4;
    clearPinBuffer();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Fill any unset positions with '0'
    for (int i = 0; i < pinLength; i++) {
      if (pinBuffer[i] == 0) pinBuffer[i] = '0';
    }
    pinBuffer[pinLength] = '\0';

    if (settingFirstEntry) {
      // Store first entry, ask to confirm
      memcpy(newPin, pinBuffer, sizeof(pinBuffer));
      settingFirstEntry = false;
      clearPinBuffer();
      showMessage("Confirm PIN");
    } else {
      // Confirm entry: compare
      if (strncmp(pinBuffer, newPin, pinLength) == 0) {
        uint8_t hash[32];
        hashPin(newPin, hash);

        if (state == SET_PIN) {
          memcpy(storedPinHash, hash, 32);
          flags |= 0x01;  // enable PIN
        } else {
          // SET_DURESS_PIN
          memcpy(storedDuressHash, hash, 32);
          flags |= 0x02;  // enable duress PIN
        }
        saveConfig();
        showMessage("PIN saved");

        // Return to settings after brief message
        state = SETTINGS_MENU;
        settingFirstEntry = true;
        clearPinBuffer();
        memset(newPin, 0, sizeof(newPin));
      } else {
        showMessage("PINs don't match");
        settingFirstEntry = true;
        clearPinBuffer();
        memset(newPin, 0, sizeof(newPin));
      }
    }
  }
}

void SecurityPinActivity::handleSettingsMenu() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (menuIndex > 0) { menuIndex--; requestUpdate(); }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (menuIndex < MENU_ITEMS - 1) { menuIndex++; requestUpdate(); }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (menuIndex) {
      case 0:  // Change PIN
        state = SET_PIN;
        settingFirstEntry = true;
        pinLength = 4;
        clearPinBuffer();
        memset(newPin, 0, sizeof(newPin));
        requestUpdate();
        break;
      case 1:  // Set Duress PIN
        state = SET_DURESS_PIN;
        settingFirstEntry = true;
        pinLength = 4;
        clearPinBuffer();
        memset(newPin, 0, sizeof(newPin));
        requestUpdate();
        break;
      case 2:  // Toggle Auto-Wipe
        flags ^= 0x04;
        saveConfig();
        requestUpdate();
        break;
      case 3:  // Disable PIN
        flags &= ~0x01;
        flags &= ~0x02;
        flags &= ~0x04;
        saveConfig();
        finish();
        break;
      case 4:  // Back
        finish();
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SecurityPinActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case ENTER_PIN:      renderPinEntry();    break;
    case SET_PIN:
    case SET_DURESS_PIN: renderSetPin();      break;
    case SETTINGS_MENU:  renderSettingsMenu(); break;
  }

  renderer.displayBuffer();
}

// Draw a row of digit entry boxes centered on screen
void SecurityPinActivity::renderPinBoxes(int y, int length, int activePos, const char* digits) const {
  const int pageWidth = renderer.getScreenWidth();
  constexpr int BOX_SIZE = 56;
  constexpr int BOX_GAP  = 10;

  const int totalW = length * BOX_SIZE + (length - 1) * BOX_GAP;
  int startX = (pageWidth - totalW) / 2;

  for (int i = 0; i < length; i++) {
    int bx = startX + i * (BOX_SIZE + BOX_GAP);
    bool active = (i == activePos);

    if (active) {
      renderer.fillRect(bx, y, BOX_SIZE, BOX_SIZE, true);
    } else {
      renderer.drawRect(bx, y, BOX_SIZE, BOX_SIZE, true);
    }

    // Draw digit character if set
    char c = digits[i];
    if (c >= '0' && c <= '9') {
      char buf[2] = {c, '\0'};
      int tw = renderer.getTextWidth(UI_12_FONT_ID, buf, EpdFontFamily::BOLD);
      int th = renderer.getLineHeight(UI_12_FONT_ID);
      int tx = bx + (BOX_SIZE - tw) / 2;
      int ty = y + (BOX_SIZE - th) / 2;
      // Invert text color on active (filled) box
      renderer.drawText(UI_12_FONT_ID, tx, ty, buf, !active, EpdFontFamily::BOLD);
    }
  }
}

void SecurityPinActivity::renderPinEntry() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Enter PIN");

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int boxY = headerBottom + (pageHeight - headerBottom - metrics.buttonHintsHeight - 56) / 2;

  renderPinBoxes(boxY, pinLength, pinPos, pinBuffer);

  // Show attempt count if any failures
  if (failCount > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "Attempt %d of %d", failCount + 1, MAX_FAILS);
    renderer.drawCenteredText(SMALL_FONT_ID, boxY + 56 + 16, buf);
  }

  // Timed message (e.g. "Wrong PIN")
  if (msgUntilMs && millis() < msgUntilMs) {
    renderer.drawCenteredText(UI_10_FONT_ID, boxY + 56 + 44, msgBuf);
  }

  const auto labels = mappedInput.mapLabels("", "Confirm", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Up", "Down");
}

void SecurityPinActivity::renderSetPin() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const char* title = (state == SET_PIN) ? "Set PIN" : "Set Duress PIN";
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int boxY = headerBottom + (pageHeight - headerBottom - metrics.buttonHintsHeight - 56) / 2;

  // Sub-header: which step
  const char* stepLabel = settingFirstEntry ? "New PIN" : "Confirm PIN";
  renderer.drawCenteredText(UI_10_FONT_ID, boxY - 36, stepLabel);

  renderPinBoxes(boxY, pinLength, pinPos, pinBuffer);

  // Length hint
  char lenHint[24];
  snprintf(lenHint, sizeof(lenHint), "%d digits  [PgFwd: change]", pinLength);
  renderer.drawCenteredText(SMALL_FONT_ID, boxY + 56 + 16, lenHint);

  // Timed message
  if (msgUntilMs && millis() < msgUntilMs) {
    renderer.drawCenteredText(UI_10_FONT_ID, boxY + 56 + 44, msgBuf);
  }

  const auto labels = mappedInput.mapLabels("Back", "Confirm", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "Up", "Down");
}

void SecurityPinActivity::renderSettingsMenu() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "PIN Security");

  const int listTop    = metrics.topPadding + metrics.headerHeight;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight;

  static constexpr const char* LABELS[MENU_ITEMS] = {
      "Change PIN",
      "Set Duress PIN",
      "Toggle Auto-Wipe",
      "Disable PIN",
      "Back",
  };

  GUI.drawList(
      renderer,
      Rect{0, listTop, pageWidth, listHeight},
      MENU_ITEMS,
      menuIndex,
      [](int i) -> std::string { return LABELS[i]; },
      [this](int i) -> std::string {
        if (i == 2) {
          return (flags & 0x04) ? "ON" : "OFF";
        }
        if (i == 1) {
          return (flags & 0x02) ? "set" : "not set";
        }
        return "";
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
