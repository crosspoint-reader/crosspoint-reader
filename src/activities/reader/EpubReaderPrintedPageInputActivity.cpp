#include "EpubReaderPrintedPageInputActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int EpubReaderPrintedPageInputActivity::powTen(int exponent) {
  int result = 1;
  for (int i = 0; i < exponent; i++) result *= 10;
  return result;
}

int EpubReaderPrintedPageInputActivity::digitCount() const {
  int n = (value > 0) ? value : 1;
  int count = 0;
  while (n > 0) {
    count++;
    n /= 10;
  }
  return count;
}

int EpubReaderPrintedPageInputActivity::maxCursorDigit() const {
  // Bound the reachable cursor position by maxValue's digit count, not the current value's.
  // This lets the user step into "empty" higher digits (e.g. cursor sits over the tens
  // place while value is still 1, and pressing Up turns it into 11). Without this you
  // could never grow a 1 into a 3-digit number without first single-pressing into double
  // digits, which defeats the point of having a digit cursor.
  int n = (maxValue > 0) ? maxValue : 1;
  int count = 0;
  while (n > 0) {
    count++;
    n /= 10;
  }
  return count - 1;  // 0-based: ones=0, tens=1, hundreds=2, ...
}

void EpubReaderPrintedPageInputActivity::clampValue() {
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
}

void EpubReaderPrintedPageInputActivity::adjustDigit(int delta) {
  value += delta * powTen(cursorDigit);
  clampValue();
  // Don't pull the cursor in toward the new digit count — leave it where the user put it.
  // The cursor is a position the user navigated to, not a property of the value.
  if (cursorDigit > maxCursorDigit()) cursorDigit = maxCursorDigit();
  requestUpdate();
}

void EpubReaderPrintedPageInputActivity::adjustDigitTimes(int multiplier, int sign) {
  // Used by the double-click handler: step is multiplier × 10^cursorDigit (e.g. 10 at the
  // ones place, 100 at the tens place). Sign is +1 or -1.
  value += sign * multiplier * powTen(cursorDigit);
  clampValue();
  if (cursorDigit > maxCursorDigit()) cursorDigit = maxCursorDigit();
  requestUpdate();
}

void EpubReaderPrintedPageInputActivity::moveCursor(int delta) {
  cursorDigit += delta;
  if (cursorDigit < 0) cursorDigit = 0;
  if (cursorDigit > maxCursorDigit()) cursorDigit = maxCursorDigit();
  requestUpdate();
}

void EpubReaderPrintedPageInputActivity::onEnter() {
  Activity::onEnter();
  // Force the FSM to wait for the double-click window on Up/Down so we can distinguish
  // Short (±1) from Double (±10). Adds ~300ms latency to single Up/Down presses; that's
  // the price for the larger step. Back/Confirm/Left/Right stay on the immediate
  // wasPressed path — no latency for navigation.
  buttonEvents.forceDoubleAction(MappedInputManager::Button::Up, true);
  buttonEvents.forceDoubleAction(MappedInputManager::Button::Down, true);
  requestUpdate();
}

void EpubReaderPrintedPageInputActivity::onExit() {
  buttonEvents.forceDoubleAction(MappedInputManager::Button::Up, false);
  buttonEvents.forceDoubleAction(MappedInputManager::Button::Down, false);
  Activity::onExit();
}

void EpubReaderPrintedPageInputActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    setResult(PrintedPageResult{std::to_string(value)});
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveCursor(1);  // cursor moves toward higher digits on Left, matching screen layout
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveCursor(-1);
    return;
  }

  // Up/Down come through the FSM-backed event queue so we can react to Short vs Double.
  // Short = ±1, Double = ±10. Both Up and PageBack map to the same hardware button; the
  // FSM emits an event for each logical button, but we only handle the Up/Down variants
  // here. The global dispatcher consumes PageBack/PageForward variants (they're configured
  // as page-turn actions in the reader) and dispatch is a no-op while this dialog is
  // current, so they're effectively swallowed.
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Up) {
      if (ev.type == ButtonEventManager::PressType::Short) {
        adjustDigit(1);
      } else if (ev.type == ButtonEventManager::PressType::Double) {
        adjustDigitTimes(10, 1);
      }
    } else if (ev.button == MappedInputManager::Button::Down) {
      if (ev.type == ButtonEventManager::PressType::Short) {
        adjustDigit(-1);
      } else if (ev.type == ButtonEventManager::PressType::Double) {
        adjustDigitTimes(10, -1);
      }
    }
    // Other queued events (PageBack/PageForward variants and any stray) are discarded;
    // the wasPressed path above already handled Back/Confirm/Left/Right.
  }
}

void EpubReaderPrintedPageInputActivity::render(RenderLock&&) {
  renderer.clearScreen();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_GO_TO_PRINTED_PAGE), true, EpdFontFamily::BOLD);

  // Big centred numeric value with an underline under the active digit position.
  // When the cursor sits over a digit position past the current value (e.g. cursor at the
  // tens place while value is still 1), we pad the displayed number with leading "·" dots
  // so the underline can mark the empty position it'll grow into on the next Up press.
  const std::string rawValueText = std::to_string(value);
  const int visibleDigits = std::max(digitCount(), cursorDigit + 1);
  std::string valueText;
  for (int i = 0; i < visibleDigits - digitCount(); i++) valueText += "0";  // leading zeros
  valueText += rawValueText;
  const int valueY = 110;
  renderer.drawCenteredText(UI_12_FONT_ID, valueY, valueText.c_str(), true, EpdFontFamily::BOLD);

  // Place the underline under the active digit. Width-based positioning approximates a
  // monospaced grid using the total rendered width / digit count; small visual mismatch on
  // proportional fonts is acceptable for a one-character indicator.
  const int totalWidth = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str());
  const int screenWidth = renderer.getScreenWidth();
  const int startX = (screenWidth - totalWidth) / 2;
  const int digitIndexFromLeft = visibleDigits - 1 - cursorDigit;  // 0-based, from the left
  const int avgDigitWidth = (visibleDigits > 0) ? totalWidth / visibleDigits : 0;
  const int underlineX = startX + digitIndexFromLeft * avgDigitWidth;
  const int underlineWidth = avgDigitWidth;
  const int underlineY = valueY + renderer.getLineHeight(UI_12_FONT_ID) + 2;
  renderer.fillRect(underlineX, underlineY, underlineWidth, 3, true);

  // Range hint underneath: "Range: 1 - 305"
  char rangeBuf[48];
  snprintf(rangeBuf, sizeof(rangeBuf), tr(STR_GO_TO_PRINTED_PAGE_RANGE), (unsigned)minValue, (unsigned)maxValue);
  renderer.drawCenteredText(SMALL_FONT_ID, underlineY + 25, rangeBuf, true);

  // Step hint.
  renderer.drawCenteredText(SMALL_FONT_ID, underlineY + 50, tr(STR_GO_TO_PRINTED_PAGE_HINT), true);

  // Button hints.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
