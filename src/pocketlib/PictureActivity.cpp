// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "PictureActivity.h"

#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"

void PictureActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y) || mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    finish();
  }
}

void PictureActivity::render(RenderLock&&) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  renderer.clearScreen();

  ImageDimensions dims{0, 0};
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoderForFile(imagePath_);
  if (!decoder || !decoder->getDimensions(imagePath_, dims) || dims.width <= 0 || dims.height <= 0) {
    LOG_ERR("PLIB", "picture %s: cannot read", imagePath_.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, screenH / 2, "This picture cannot be shown");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // As large as fits, at most three times its own size (beyond that a small
  // picture is only blocks).
  constexpr int kMargin = 8;
  const float fit = std::min(static_cast<float>(screenW - 2 * kMargin) / dims.width,
                             static_cast<float>(screenH - 2 * kMargin) / dims.height);
  const float scale = std::min(fit, 3.0f);
  const int w = std::max(1, static_cast<int>(dims.width * scale));
  const int h = std::max(1, static_cast<int>(dims.height * scale));
  const int x = (screenW - w) / 2;
  const int y = (screenH - h) / 2;

  // The page's own cache of this picture is at its page size: this decodes it
  // afresh at this size (and the page decodes again on return). A failure
  // remembered from the page is tried again here.
  ImageBlock::releaseRenderCache();
  ImageBlock::clearRenderFailures();
  ImageBlock block(imagePath_, std::string(), static_cast<int16_t>(w), static_cast<int16_t>(h));
  block.render(renderer, x, y);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  ReaderUtils::renderAntiAliased(renderer, [&]() { block.render(renderer, x, y); });
  ImageBlock::releaseRenderCache();
}

#endif  // POCKET_LIBRARY
