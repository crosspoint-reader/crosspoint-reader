// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <GfxRenderer.h>

#include <cstdint>

namespace pocketlib {

// Draws one of our icons (libraryIcons.h), or a picture stored the same way
// (drawUprightBitmap), the right way up. gen_icons.py
// writes them upright (row by row, MSB first, 0 = ink) for freeink's UI;
// GfxRenderer::drawIcon expects CrossPoint's icons, which are stored turned
// a quarter turn, so it showed ours on their side. Plotted pixel by pixel,
// like drawIcon, so the position is exact.
inline void drawUprightBitmap(const GfxRenderer& renderer, const uint8_t* bits, const int x, const int y,
                              const int width, const int height) {
  const int rowBytes = (width + 7) / 8;
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++) {
      const uint8_t byte = bits[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) renderer.drawPixel(x + col, y + row, true);
    }
  }
}

inline void drawUprightIcon(const GfxRenderer& renderer, const uint8_t* bits, const int x, const int y,
                            const int size) {
  drawUprightBitmap(renderer, bits, x, y, size, size);
}

}  // namespace pocketlib
