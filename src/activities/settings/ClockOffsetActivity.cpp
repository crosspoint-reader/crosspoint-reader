#include "ClockOffsetActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint8_t MAX_POS_HOURS = 14;
constexpr uint8_t MAX_NEG_HOURS = 12;
constexpr uint8_t MINUTE_STEPS = 4;  // 0, 15, 30, 45
constexpr uint8_t MINUTES_PER_QUARTER = 15;
constexpr uint8_t BIAS_QUARTER_HOURS = 48;  // 0 stored = UTC-12, 48 stored = UTC+0

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

void ClockOffsetActivity::onEnter() {
  Activity::onEnter();
  loadFromSettings();
  activeField = FIELD_HOURS;
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

void ClockOffsetActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_UTC_OFFSET));

  // Build the offset string. Use a generous font and centre it.
  char offsetBuf[16];
  snprintf(offsetBuf, sizeof(offsetBuf), "UTC %c %d:%02d", sign == 1 ? '-' : '+', hours,
           minutesQuarter * MINUTES_PER_QUARTER);

  const int centreY = pageHeight / 2 - 40;
  renderer.drawCenteredText(UI_12_FONT_ID, centreY, offsetBuf, true, EpdFontFamily::BOLD);

  // Underline / caret under the active field. Compute positions by measuring substrings of the
  // formatted string so the caret follows the font glyph widths exactly.
  // Field substrings:
  //   "UTC "        -> prefix
  //   "{+/-}"       -> sign
  //   " "
  //   "{hours}"     -> hours
  //   ":"
  //   "{mm}"        -> minutes
  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s); };
  const int totalWidth = widthOf(offsetBuf);
  const int leftEdge = (pageWidth - totalWidth) / 2;

  // Locate each field by reformatting prefixes.
  char prefixSign[16];
  snprintf(prefixSign, sizeof(prefixSign), "UTC ");
  const int signX = leftEdge + widthOf(prefixSign);

  char prefixHours[16];
  snprintf(prefixHours, sizeof(prefixHours), "UTC %c ", sign == 1 ? '-' : '+');
  const int hoursX = leftEdge + widthOf(prefixHours);

  char prefixMinutes[16];
  snprintf(prefixMinutes, sizeof(prefixMinutes), "UTC %c %d:", sign == 1 ? '-' : '+', hours);
  const int minutesX = leftEdge + widthOf(prefixMinutes);

  // Width of each field substring for the caret span.
  const int signW = widthOf(sign == 1 ? "-" : "+");
  char hoursStr[8];
  snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
  const int hoursW = widthOf(hoursStr);
  char minutesStr[8];
  snprintf(minutesStr, sizeof(minutesStr), "%02d", minutesQuarter * MINUTES_PER_QUARTER);
  const int minutesW = widthOf(minutesStr);

  int caretX = 0;
  int caretW = 0;
  switch (activeField) {
    case FIELD_SIGN:
      caretX = signX;
      caretW = signW;
      break;
    case FIELD_HOURS:
      caretX = hoursX;
      caretW = hoursW;
      break;
    case FIELD_MINUTES:
      caretX = minutesX;
      caretW = minutesW;
      break;
    default:
      break;
  }
  // Caret drawn as a short bar below the active field.
  const int caretY = centreY + 10;
  for (int dy = 0; dy < 2; dy++) {
    renderer.drawLine(caretX, caretY + dy, caretX + caretW, caretY + dy);
  }

  // Live preview of the resulting wall-clock time, so users can verify against a watch.
  if (halClock.isAvailable()) {
    char timeBuf[9];
    const uint8_t encoded = encodeOffset(sign, hours, minutesQuarter);
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), encoded, SETTINGS.clockFormat == 1)) {
      char preview[24];
      snprintf(preview, sizeof(preview), "%s %s", tr(STR_CURRENT_TIME), timeBuf);
      renderer.drawCenteredText(UI_10_FONT_ID, centreY + 60, preview);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
