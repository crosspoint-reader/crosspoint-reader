#pragma once

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"

namespace OrientationHelper {

// Convert GfxRenderer orientation to MappedInputManager orientation.
inline MappedInputManager::Orientation toInputOrientation(GfxRenderer::Orientation o) {
  switch (o) {
    case GfxRenderer::Orientation::PortraitInverted:
      return MappedInputManager::Orientation::PortraitInverted;
    case GfxRenderer::Orientation::LandscapeClockwise:
      return MappedInputManager::Orientation::LandscapeClockwise;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      return MappedInputManager::Orientation::LandscapeCounterClockwise;
    case GfxRenderer::Orientation::Portrait:
    default:
      return MappedInputManager::Orientation::Portrait;
  }
}

// Apply screen orientation based on global settings and activity capabilities.
// Reader activities use SETTINGS.orientation (supports all 4 directions).
// UI activities use SETTINGS.uiOrientation (Portrait or Inverted only).
// Also syncs the effective orientation to MappedInputManager so button
// mapping matches the actual screen direction.
inline void applyOrientation(GfxRenderer& renderer, MappedInputManager& input, const Activity* activity) {
  const auto readerSetting = static_cast<CrossPointSettings::ORIENTATION>(SETTINGS.orientation);
  const auto uiSetting = static_cast<CrossPointSettings::UI_ORIENTATION>(SETTINGS.uiOrientation);
  GfxRenderer::Orientation target;

  if (activity->supportsLandscape()) {
    // Reader: use full orientation setting
    switch (readerSetting) {
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
        target = GfxRenderer::Orientation::LandscapeClockwise;
        break;
      case CrossPointSettings::ORIENTATION::INVERTED:
        target = GfxRenderer::Orientation::PortraitInverted;
        break;
      case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
        target = GfxRenderer::Orientation::LandscapeCounterClockwise;
        break;
      case CrossPointSettings::ORIENTATION::PORTRAIT:
      default:
        target = GfxRenderer::Orientation::Portrait;
        break;
    }
  } else {
    // UI: only Portrait or Inverted
    target = (uiSetting == CrossPointSettings::UI_ORIENTATION::UI_INVERTED) ? GfxRenderer::Orientation::PortraitInverted
                                                                            : GfxRenderer::Orientation::Portrait;
  }

  renderer.setOrientation(target);
  input.setEffectiveOrientation(toInputOrientation(target));
}

}  // namespace OrientationHelper
