#include "ThemeShared.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <string>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace ThemeShared {

void drawFlatButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                         int bandHeight, int bottomPadding) {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int pageWidth = renderer.getScreenWidth();
  // Top of the reserved band. The rule and labels live in the upper
  // `bandHeight - bottomPadding` pixels; the remainder is left blank so the footer
  // floats above the screen edge.
  const int bandY = pageHeight - bandHeight;
  const int visualHeight = bandHeight - bottomPadding;

  // Stock slot geometry -- X3 is 528px wide in portrait against the X4's 480.
  constexpr int slotWidth = 106;
  constexpr int x4Slots[] = {25, 130, 245, 350};
  constexpr int x3Slots[] = {38, 154, 268, 384};
  const int* slots = gpio.deviceIsX3() ? x3Slots : x4Slots;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  renderer.fillRect(0, bandY, pageWidth, bandHeight, false);
  renderer.drawLine(0, bandY, pageWidth - 1, bandY, true);

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = bandY + (visualHeight - lineHeight) / 2 + 1;

  for (int i = 0; i < 4; ++i) {
    if (labels[i] == nullptr || labels[i][0] == '\0') {
      continue;
    }
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
    const int textX = slots[i] + (slotWidth - 1 - textWidth) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i]);
  }

  renderer.setOrientation(origOrientation);
}

void drawTitleStatusBar(const BaseTheme& theme, const GfxRenderer& renderer, Rect rect, const char* title,
                        int sidePadding, int batteryWidth, int batteryHeight, int titleFontId) {
  // Clear the band: the battery percentage shrinks as it drops from 100% to 9%, and
  // without this the wider previous value stays on screen under partial refresh.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showPercent = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;

  const int batteryY = rect.y + (rect.height - batteryHeight) / 2;
  const int batteryX = rect.x + rect.width - sidePadding - batteryWidth;
  theme.drawBatteryRight(renderer, Rect{batteryX, batteryY, batteryWidth, batteryHeight}, showPercent);

  if (title == nullptr || title[0] == '\0') {
    return;
  }

  // Reserve the icon, and when shown the percentage text that sits to its left.
  // "100%" is measured rather than the live value so the title does not reflow
  // every time the battery ticks down a digit.
  int reserved = batteryWidth + sidePadding * 2;
  if (showPercent) {
    reserved += BaseTheme::batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, "100%");
  }

  const int maxTitleWidth = rect.width - sidePadding - reserved;
  if (maxTitleWidth <= 0) {
    return;
  }

  const std::string truncated = renderer.truncatedText(titleFontId, title, maxTitleWidth, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(titleFontId);
  renderer.drawText(titleFontId, rect.x + sidePadding, rect.y + (rect.height - lineHeight) / 2, truncated.c_str(), true,
                    EpdFontFamily::BOLD);
}

}  // namespace ThemeShared
