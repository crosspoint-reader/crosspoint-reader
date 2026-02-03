#include "Activity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"

void Activity::onEnter() {
  Serial.printf("[%lu] [ACT] Entering activity: %s\n", millis(), name.c_str());
  updateMargins();
}

void Activity::onExit() { Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str()); }

void Activity::updateMargins() {
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      marginTop = 15;
      marginBottom = 50;
      marginLeft = marginRight = 20;
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      marginTop = 15;
      marginLeft = 50;
      marginRight = marginBottom = 20;
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      marginTop = 50;
      marginBottom = 15;
      marginLeft = marginRight = 20;
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      marginTop = 15;
      marginRight = 50;
      marginLeft = marginBottom = 20;
      break;
    default:
      break;
  }
  contentStartY = marginTop + 45;
}

void Activity::updateRendererOrientation() {
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
  }
  updateMargins();
}

// Number of items that fit on a page, derived from logical screen height.
// This adapts automatically when switching between portrait and landscape.
int Activity::getPageItems() const {
  // Layout constants used in renderScreen
  const int startY = contentStartY;
  constexpr int lineHeight = LINE_HEIGHT;

  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - lineHeight;

  const int availableHeight = endY - startY;
  int items = availableHeight / lineHeight;

  // Ensure we always have at least one item per page to avoid division by zero
  if (items < 1) {
    items = 1;
  }
  return items;
}
