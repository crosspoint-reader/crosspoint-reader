#include "components/UIHelpers.h"

namespace UIHelpers {

ContentArea contentAreaForRenderer(const GfxRenderer& renderer) {
  ContentArea area{};
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  area.isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  area.isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;

  const auto& metrics = UITheme::getInstance().getMetrics();
  // Use theme metrics for side/side-hint sizes and button hint height
  area.hintGutterWidth = (area.isLandscapeCw || area.isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  area.hintGutterHeight = (orientation == GfxRenderer::Orientation::PortraitInverted) ? 50 : 0;

  // Reserve side gutters: left for Landscape CW, plus the side button area on the right for landscape
  const int leftGutter = area.isLandscapeCw ? area.hintGutterWidth : 0;
  const int rightGutter = (area.isLandscapeCw || area.isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  area.contentX = leftGutter;
  area.contentWidth = pageWidth - leftGutter - rightGutter;

  // Reserve bottom space for button hints (vertical spacing + button area)
  const int reservedBottom = metrics.verticalSpacing + metrics.buttonHintsHeight;
  area.contentY = area.hintGutterHeight;
  area.contentHeight = pageHeight - area.contentY - reservedBottom;

  return area;
}

int centeredTextX(const GfxRenderer& renderer, int fontId, const std::string& text, const ContentArea& area,
                  EpdFontFamily::Style fontStyle) {
  const int textWidth = renderer.getTextWidth(fontId, text.c_str(), fontStyle);
  return area.contentX + (area.contentWidth - textWidth) / 2;
}

std::string truncatedTextForContent(const GfxRenderer& renderer, int fontId, const std::string& text,
                                    const ContentArea& area, EpdFontFamily::Style fontStyle) {
  // Reserve modest padding (40px) as used elsewhere in the UI
  const int available = std::max(8, area.contentWidth - 40);
  return renderer.truncatedText(fontId, text.c_str(), available, fontStyle);
}

}  // namespace UIHelpers
