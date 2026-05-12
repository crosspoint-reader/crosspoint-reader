#include "ClassicButtonsTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int cornerRadius = 6;
}

void ClassicButtonsTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                          const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = ClassicButtonsMetrics::values.buttonHintsHeight;
  constexpr int buttonY = ClassicButtonsMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      if (!BaseTheme::drawArrowIfNeeded(renderer, labels[i], x + buttonWidth / 2,
                                        pageHeight - buttonY + buttonHeight / 2, 5, true)) {
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textX = x + (buttonWidth - 1 - textWidth) / 2;
        renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
      }
    } else {
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}
