#include "ClockOffsetActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Field select (value = Field) and -/+ step (value = delta) touch actions.
constexpr fui::ActionId ACTION_FIELD = 1;
constexpr fui::ActionId ACTION_STEP = 2;

constexpr uint8_t MAX_POS_HOURS = 14;
constexpr uint8_t MAX_NEG_HOURS = 12;
constexpr uint8_t MINUTE_STEPS = 4;  // 0, 15, 30, 45
constexpr uint8_t MINUTES_PER_QUARTER = 15;
constexpr uint8_t BIAS_QUARTER_HOURS = 48;  // 0 stored = UTC-12, 48 stored = UTC+0
constexpr int TOUCH_BUTTON_SIZE = 44;
constexpr int TOUCH_BUTTON_GAP = 18;

// Convert a (sign, hours, quarter) triple into the biased storage value.
// Returns a value in [0, 104].
uint8_t encodeOffset(uint8_t sign, uint8_t hours, uint8_t quarter) {
  int signedQuarter = static_cast<int>(hours) * 4 + static_cast<int>(quarter);
  if (sign == 1) signedQuarter = -signedQuarter;
  int biased = signedQuarter + BIAS_QUARTER_HOURS;
  if (biased < 0) biased = 0;
  if (biased > 104) biased = 104;
  return static_cast<uint8_t>(biased);
}

// Decompose the biased storage value into (sign, hours, quarter).
void decodeOffset(uint8_t biased, uint8_t& sign, uint8_t& hours, uint8_t& quarter) {
  if (biased > 104) biased = BIAS_QUARTER_HOURS;
  int signedQuarter = static_cast<int>(biased) - BIAS_QUARTER_HOURS;
  if (signedQuarter < 0) {
    sign = 1;
    signedQuarter = -signedQuarter;
  } else {
    sign = 0;
  }
  hours = static_cast<uint8_t>(signedQuarter / 4);
  quarter = static_cast<uint8_t>(signedQuarter % 4);
}
}  // namespace

ClockOffsetActivity::ClockOffsetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ClockOffset", renderer, mappedInput), UiAppHost(renderer) {}

void ClockOffsetActivity::onEnter() {
  Activity::onEnter();
  loadFromSettings();
  activeField = FIELD_HOURS;
  resetUi();
  app.on(ACTION_FIELD, &ClockOffsetActivity::onFieldEvent, this);
  app.on(ACTION_STEP, &ClockOffsetActivity::onStepEvent, this);
  app.setScreen(&ClockOffsetActivity::offsetScreen, this);
  requestUpdate();
}

void ClockOffsetActivity::onExit() {
  saveToSettings();
  Activity::onExit();
}

void ClockOffsetActivity::loadFromSettings() {
  decodeOffset(SETTINGS.clockUtcOffsetQ, sign, hours, minutesQuarter);
  clampForSign();
}

void ClockOffsetActivity::saveToSettings() const {
  const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
  if (encoded == SETTINGS.clockUtcOffsetQ) return;
  SETTINGS.clockUtcOffsetQ = encoded;
  SETTINGS.saveToFile();
}

void ClockOffsetActivity::clampForSign() {
  const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
  if (hours > maxHours) hours = maxHours;
  // At the absolute boundary (-12:00 or +14:00) only :00 is valid.
  if (hours == maxHours && minutesQuarter != 0) {
    minutesQuarter = 0;
  }
}

void ClockOffsetActivity::adjustActiveField(int delta) {
  switch (activeField) {
    case FIELD_SIGN: {
      sign = static_cast<uint8_t>((sign + 1) % 2);
      clampForSign();
      break;
    }
    case FIELD_HOURS: {
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      const int next = (static_cast<int>(hours) + delta + (maxHours + 1)) % (maxHours + 1);
      hours = static_cast<uint8_t>(next);
      clampForSign();
      break;
    }
    case FIELD_MINUTES: {
      // At the boundary hour, lock minutes to :00.
      const uint8_t maxHours = (sign == 1) ? MAX_NEG_HOURS : MAX_POS_HOURS;
      if (hours == maxHours) {
        minutesQuarter = 0;
        break;
      }
      const int next = (static_cast<int>(minutesQuarter) + delta + MINUTE_STEPS) % MINUTE_STEPS;
      minutesQuarter = static_cast<uint8_t>(next);
      break;
    }
    default:
      break;
  }
}

void ClockOffsetActivity::getFieldRects(Rect& signRect, Rect& hoursRect, Rect& minutesRect) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int centreY = pageHeight / 2 - 40;
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };
  constexpr int fieldPaddingX = 6;
  constexpr int labelGap = 16;
  constexpr int fieldGap = 12;
  constexpr int colonGap = 5;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int fieldHeight = lineHeight + 2;

  const int labelWidth = widthOf("UTC");
  const int signBoxW = std::max(widthOf("+"), widthOf("-")) + fieldPaddingX * 2;
  const int hoursBoxW = std::max(widthOf("14"), widthOf("12")) + fieldPaddingX * 2;
  const int colonWidth = widthOf(":");
  const int minutesBoxW = std::max({widthOf("00"), widthOf("15"), widthOf("30"), widthOf("45")}) + fieldPaddingX * 2;
  const int totalWidth =
      labelWidth + labelGap + signBoxW + fieldGap + hoursBoxW + colonGap + colonWidth + colonGap + minutesBoxW;

  int boxX = (pageWidth - totalWidth) / 2 + labelWidth + labelGap;
  signRect = Rect{boxX, centreY, signBoxW, fieldHeight};
  boxX += signBoxW + fieldGap;
  hoursRect = Rect{boxX, centreY, hoursBoxW, fieldHeight};
  boxX += hoursBoxW + colonGap + colonWidth + colonGap;
  minutesRect = Rect{boxX, centreY, minutesBoxW, fieldHeight};
}

void ClockOffsetActivity::getTouchControlRects(Rect& minusRect, Rect& plusRect) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int centreY = pageHeight / 2 - 40;
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };
  constexpr int fieldPaddingX = 6;
  constexpr int labelGap = 16;
  constexpr int fieldGap = 12;
  constexpr int colonGap = 5;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int fieldHeight = lineHeight + 2;
  const int labelWidth = widthOf("UTC");
  const int signBoxW = std::max(widthOf("+"), widthOf("-")) + fieldPaddingX * 2;
  const int hoursBoxW = std::max(widthOf("14"), widthOf("12")) + fieldPaddingX * 2;
  const int colonWidth = widthOf(":");
  const int minutesBoxW = std::max({widthOf("00"), widthOf("15"), widthOf("30"), widthOf("45")}) + fieldPaddingX * 2;
  const int totalWidth =
      labelWidth + labelGap + signBoxW + fieldGap + hoursBoxW + colonGap + colonWidth + colonGap + minutesBoxW;
  const int offsetX = (pageWidth - totalWidth) / 2;
  const int buttonY = centreY + (fieldHeight - TOUCH_BUTTON_SIZE) / 2;
  minusRect = Rect{offsetX - TOUCH_BUTTON_GAP - TOUCH_BUTTON_SIZE, buttonY, TOUCH_BUTTON_SIZE, TOUCH_BUTTON_SIZE};
  plusRect = Rect{offsetX + totalWidth + TOUCH_BUTTON_GAP, buttonY, TOUCH_BUTTON_SIZE, TOUCH_BUTTON_SIZE};
}

void ClockOffsetActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activeField = static_cast<Field>((activeField + 1) % FIELD_COUNT);
    requestUpdate();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the field and -/+
  // hit rects. Fields carry InputDrag so contact (held frames) selects them as
  // soon as the finger lands, matching the old touch-down behaviour, while
  // taps on -/+ and the sign toggle dispatch on release. Handlers
  // requestUpdate themselves, so the app's invalidation flag is deliberately
  // ignored: held frames dispatch every pass and must not repaint an
  // unchanged screen.
  if (routingReady() && mappedInput.hasTouch()) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      routedRelease = snap.touchReleased;
      if (route(snap)) {
        return;
      }
    }
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void ClockOffsetActivity::onFieldEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ClockOffsetActivity*>(user);
  const Field touched = static_cast<Field>(event.value);
  if (self->routedRelease) {
    // Tap: the sign field toggles, the other fields become active.
    if (touched == FIELD_SIGN) {
      self->activeField = FIELD_SIGN;
      self->adjustActiveField(+1);
    } else {
      self->activeField = touched;
    }
    self->requestUpdate();
    return;
  }
  // Contact (touch-down / held): select the field under the finger.
  if (self->activeField != touched) {
    self->activeField = touched;
    self->requestUpdate();
  }
}

void ClockOffsetActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ClockOffsetActivity*>(user);
  self->adjustActiveField(event.value);
  self->requestUpdate();
}

void ClockOffsetActivity::offsetScreen(UiScreen& screen, void* user) {
  static_cast<ClockOffsetActivity*>(user)->buildOffsetScreen(screen);
}

void ClockOffsetActivity::buildOffsetScreen(UiScreen& screen) {
  // All visuals stay on the legacy renderer draws in render(); this screen
  // only registers the touch hit rects over them, so button-only boards (which
  // also skip the -/+ buttons) register nothing.
  if (!mappedInput.hasTouch()) return;

  auto toFui = [](const Rect& rect) { return fui::makeRect(rect.x, rect.y, rect.width, rect.height); };

  Rect signRect;
  Rect hoursRect;
  Rect minutesRect;
  getFieldRects(signRect, hoursRect, minutesRect);
  // InputDrag: contact binds and dispatches while the finger is down, so the
  // field selects on touch-down like the old hand-rolled hit test did.
  // (const, not constexpr: the SDK's InputMask operator| is a plain inline.)
  const uint16_t fieldMask = static_cast<uint16_t>(fui::InputTouch | fui::InputDrag);
  screen.frame().hit(toFui(signRect), ACTION_FIELD, FIELD_SIGN, fieldMask);
  screen.frame().hit(toFui(hoursRect), ACTION_FIELD, FIELD_HOURS, fieldMask);
  screen.frame().hit(toFui(minutesRect), ACTION_FIELD, FIELD_MINUTES, fieldMask);

  Rect minusRect;
  Rect plusRect;
  getTouchControlRects(minusRect, plusRect);
  screen.frame().hit(toFui(minusRect), ACTION_STEP, -1, fui::InputTouch);
  screen.frame().hit(toFui(plusRect), ACTION_STEP, +1, fui::InputTouch);
}

void ClockOffsetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_UTC_OFFSET));

  const int centreY = pageHeight / 2 - 40;
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s, EpdFontFamily::BOLD); };
  constexpr int fieldPaddingX = 6;
  constexpr int labelGap = 16;
  constexpr int fieldGap = 12;
  constexpr int colonGap = 5;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int fieldHeight = lineHeight + 2;

  char signStr[2] = {sign == 1 ? '-' : '+', '\0'};
  char hoursStr[8];
  snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
  char minutesStr[8];
  snprintf(minutesStr, sizeof(minutesStr), "%02d", minutesQuarter * MINUTES_PER_QUARTER);

  const int labelWidth = widthOf("UTC");
  const int signBoxW = std::max(widthOf("+"), widthOf("-")) + fieldPaddingX * 2;
  const int hoursBoxW = std::max(widthOf("14"), widthOf("12")) + fieldPaddingX * 2;
  const int colonWidth = widthOf(":");
  const int minutesBoxW = std::max({widthOf("00"), widthOf("15"), widthOf("30"), widthOf("45")}) + fieldPaddingX * 2;
  const int totalWidth =
      labelWidth + labelGap + signBoxW + fieldGap + hoursBoxW + colonGap + colonWidth + colonGap + minutesBoxW;

  int x = (pageWidth - totalWidth) / 2;
  renderer.drawText(UI_12_FONT_ID, x, centreY, "UTC", true, EpdFontFamily::BOLD);
  x += labelWidth + labelGap;

  auto drawField = [&](const char* text, const int boxX, const int boxWidth, const Field field) {
    const bool selected = activeField == field;
    renderer.fillRectDither(boxX, centreY, boxWidth, fieldHeight, selected ? Color::LightGray : Color::White);
    renderer.drawRect(boxX, centreY, boxWidth, fieldHeight, true);
    if (selected) {
      renderer.drawRect(boxX + 1, centreY + 1, boxWidth - 2, fieldHeight - 2, true);
    }
    const int textX = boxX + (boxWidth - widthOf(text)) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, centreY, text, true, EpdFontFamily::BOLD);
  };

  drawField(signStr, x, signBoxW, FIELD_SIGN);
  x += signBoxW + fieldGap;

  drawField(hoursStr, x, hoursBoxW, FIELD_HOURS);
  x += hoursBoxW + colonGap;

  renderer.drawText(UI_12_FONT_ID, x, centreY, ":", true, EpdFontFamily::BOLD);
  x += colonWidth + colonGap;

  drawField(minutesStr, x, minutesBoxW, FIELD_MINUTES);

  if (mappedInput.hasTouch()) {
    Rect minusRect;
    Rect plusRect;
    getTouchControlRects(minusRect, plusRect);
    auto drawTouchButton = [&](const Rect& rect, const char* label) {
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::White);
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
      const int textX = rect.x + (rect.width - widthOf(label)) / 2;
      const int textY = rect.y + (rect.height - lineHeight) / 2;
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, true, EpdFontFamily::BOLD);
    };
    drawTouchButton(minusRect, "-");
    drawTouchButton(plusRect, "+");
  }

  // Live preview of the resulting wall-clock time, so users can verify against a watch.
  if (halClock.isAvailable()) {
    char timeBuf[9];
    const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), encoded, SETTINGS.clockFormat == 1)) {
      // 24 bytes did not even hold the label itself once translated: STR_CURRENT_TIME
      // is 26 bytes in Russian, 24 in Arabic and Ukrainian. See ClockSyncActivity.
      char preview[64];
      snprintf(preview, sizeof(preview), "%s %s", tr(STR_CURRENT_TIME), timeBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, centreY + 60, preview);
    }
  }

  // Rebuild the app's touch hit rects over the legacy-drawn controls (the
  // screen builder draws nothing, so the visuals above are untouched).
  renderUi();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
