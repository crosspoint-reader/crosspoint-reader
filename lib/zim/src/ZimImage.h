// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Article images for an e-ink screen: Kiwix stores Wikipedia's images as
// WebP, which the reader's image pipeline doesn't read. This decodes one
// (libwebp, scaled while decoding so a large image never sits in memory at
// full size), puts it on white where it is transparent, turns it to grey,
// and writes an 8-bit greyscale PNG the reader's PNG decoder can show.
// The PNG is stored uncompressed (deflate "stored" blocks): it lives briefly
// on the card, and skipping compression keeps the device's work to a copy.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zim {

struct ImageSize {
  int width = 0;
  int height = 0;
};

// The image's own size from its header; false if it isn't a WebP image.
bool webpSize(const uint8_t* data, size_t size, ImageSize& out);

// Size to draw at: the image's own, shrunk (never enlarged) to fit.
ImageSize fitWithin(ImageSize image, int maxWidth, int maxHeight);

// Decodes `data` scaled to fit maxWidth x maxHeight and appends a greyscale
// PNG of it to `png`. False on a corrupt image or no memory.
bool webpToGrayPng(const uint8_t* data, size_t size, int maxWidth, int maxHeight, std::vector<uint8_t>& png,
                   ImageSize* written = nullptr);

// An 8-bit greyscale PNG of `gray` (width x height, rows top to bottom).
void writeGrayPng(const uint8_t* gray, int width, int height, std::vector<uint8_t>& png);

}  // namespace zim
