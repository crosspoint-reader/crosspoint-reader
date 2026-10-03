// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimImage.h"

#include <src/webp/decode.h>

#include <algorithm>
#include <cstring>

namespace zim {
namespace {

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

// Length, type, data, CRC of type and data.
void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
  put32(out, static_cast<uint32_t>(size));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  if (size) out.insert(out.end(), data, data + size);
  put32(out, crc32(out.data() + start, size + 4));
}

}  // namespace

bool webpSize(const uint8_t* data, size_t size, ImageSize& out) {
  int w = 0;
  int h = 0;
  if (!data || !WebPGetInfo(data, size, &w, &h)) return false;
  out = {w, h};
  return true;
}

ImageSize fitWithin(ImageSize image, int maxWidth, int maxHeight) {
  if (image.width <= 0 || image.height <= 0) return {};
  double scale = 1.0;
  if (maxWidth > 0 && image.width > maxWidth) scale = std::min(scale, static_cast<double>(maxWidth) / image.width);
  if (maxHeight > 0 && image.height > maxHeight) scale = std::min(scale, static_cast<double>(maxHeight) / image.height);
  return {std::max(1, static_cast<int>(image.width * scale + 0.5)),
          std::max(1, static_cast<int>(image.height * scale + 0.5))};
}

void writeGrayPng(const uint8_t* gray, int width, int height, std::vector<uint8_t>& png) {
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  png.insert(png.end(), kSignature, kSignature + 8);

  uint8_t ihdr[13];
  const uint32_t w = static_cast<uint32_t>(width);
  const uint32_t h = static_cast<uint32_t>(height);
  const uint8_t head[13] = {static_cast<uint8_t>(w >> 24),
                            static_cast<uint8_t>(w >> 16),
                            static_cast<uint8_t>(w >> 8),
                            static_cast<uint8_t>(w),
                            static_cast<uint8_t>(h >> 24),
                            static_cast<uint8_t>(h >> 16),
                            static_cast<uint8_t>(h >> 8),
                            static_cast<uint8_t>(h),
                            8,  // bit depth
                            0,  // colour type: greyscale
                            0,
                            0,
                            0};
  std::memcpy(ihdr, head, sizeof(ihdr));
  chunk(png, "IHDR", ihdr, sizeof(ihdr));

  // zlib stream of "stored" deflate blocks; each row starts with filter 0.
  const size_t raw = static_cast<size_t>(height) * (static_cast<size_t>(width) + 1);
  std::vector<uint8_t> z;
  z.reserve(raw + raw / 65535 * 5 + 16);
  z.push_back(0x78);
  z.push_back(0x01);
  uint32_t a = 1;
  uint32_t b = 0;
  size_t done = 0;
  size_t row = 0;
  size_t col = 0;  // position within the current row, 0 = the filter byte
  while (done < raw) {
    const size_t n = std::min<size_t>(raw - done, 65535);
    const bool last = done + n == raw;
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    for (size_t i = 0; i < n; i++) {
      const uint8_t v = col == 0 ? 0 : gray[row * static_cast<size_t>(width) + (col - 1)];
      z.push_back(v);
      a = (a + v) % 65521;
      b = (b + a) % 65521;
      if (++col > static_cast<size_t>(width)) {
        col = 0;
        row++;
      }
    }
    done += n;
  }
  put32(z, (b << 16) | a);
  chunk(png, "IDAT", z.data(), z.size());
  chunk(png, "IEND", nullptr, 0);
}

bool webpToGrayPng(const uint8_t* data, size_t size, int maxWidth, int maxHeight, std::vector<uint8_t>& png,
                   ImageSize* written) {
  WebPDecoderConfig config;
  if (!WebPInitDecoderConfig(&config)) return false;
  if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return false;
  const ImageSize target = fitWithin({config.input.width, config.input.height}, maxWidth, maxHeight);
  if (target.width <= 0) return false;
  if (target.width != config.input.width || target.height != config.input.height) {
    config.options.use_scaling = 1;
    config.options.scaled_width = target.width;
    config.options.scaled_height = target.height;
  }
  config.output.colorspace = MODE_RGBA;
  if (WebPDecode(data, size, &config) != VP8_STATUS_OK) {
    WebPFreeDecBuffer(&config.output);
    return false;
  }
  const WebPRGBABuffer& rgba = config.output.u.RGBA;
  const int w = config.output.width;
  const int h = config.output.height;
  std::vector<uint8_t> gray;
  gray.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int y = 0; y < h; y++) {
    const uint8_t* p = rgba.rgba + static_cast<size_t>(y) * rgba.stride;
    uint8_t* g = gray.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x++, p += 4) {
      // Rec. 601 luma, then transparent parts on white paper.
      const uint32_t luma = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
      g[x] = static_cast<uint8_t>((luma * p[3] + 255u * (255u - p[3])) / 255u);
    }
  }
  WebPFreeDecBuffer(&config.output);
  writeGrayPng(gray.data(), w, h, png);
  if (written) *written = {w, h};
  return true;
}

}  // namespace zim
