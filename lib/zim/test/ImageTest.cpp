// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Every WebP image in openZIM's Wikipedia sample, decoded and written as a
// greyscale PNG, checked by reading the PNG back.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "PosixSource.h"
#include "ZimArchive.h"
#include "ZimImage.h"

namespace {

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Reads back what writeGrayPng writes: signature, IHDR, one IDAT of stored
// deflate blocks with a correct Adler-32, IEND. Returns the pixels.
bool readGrayPng(const std::vector<uint8_t>& png, int& w, int& h, std::vector<uint8_t>& pixels) {
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (png.size() < 8 + 25 + 12 + 12 || std::memcmp(png.data(), kSig, 8) != 0) return false;
  size_t p = 8;
  std::vector<uint8_t> z;
  bool end = false;
  while (p + 12 <= png.size() && !end) {
    const uint32_t len = be32(&png[p]);
    const std::string type(reinterpret_cast<const char*>(&png[p + 4]), 4);
    const uint8_t* data = &png[p + 8];
    if (type == "IHDR") {
      w = static_cast<int>(be32(data));
      h = static_cast<int>(be32(data + 4));
      if (data[8] != 8 || data[9] != 0) return false;
    } else if (type == "IDAT") {
      z.insert(z.end(), data, data + len);
    } else if (type == "IEND") {
      end = true;
    }
    p += 12 + len;
  }
  if (!end || z.size() < 6) return false;
  std::vector<uint8_t> raw;
  size_t q = 2;
  for (;;) {
    const bool last = z[q] & 1;
    const size_t n = z[q + 1] | (z[q + 2] << 8);
    if (((z[q + 3] | (z[q + 4] << 8)) ^ 0xFFFF) != n) return false;
    raw.insert(raw.end(), z.begin() + q + 5, z.begin() + q + 5 + n);
    q += 5 + n;
    if (last) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  if (be32(&z[q]) != ((b << 16) | a)) return false;
  if (raw.size() != static_cast<size_t>(h) * (w + 1)) return false;
  pixels.clear();
  for (int y = 0; y < h; y++) {
    if (raw[y * (w + 1)] != 0) return false;
    pixels.insert(pixels.end(), raw.begin() + y * (w + 1) + 1, raw.begin() + (y + 1) * (w + 1));
  }
  return true;
}

TEST(Image, GrayPngRoundTrips) {
  std::vector<uint8_t> gray(300 * 250);
  for (size_t i = 0; i < gray.size(); i++) gray[i] = static_cast<uint8_t>(i * 7);
  std::vector<uint8_t> png;
  zim::writeGrayPng(gray.data(), 300, 250, png);  // more than one 64 KB stored block
  int w = 0, h = 0;
  std::vector<uint8_t> back;
  ASSERT_TRUE(readGrayPng(png, w, h, back));
  EXPECT_EQ(w, 300);
  EXPECT_EQ(h, 250);
  EXPECT_EQ(back, gray);
}

TEST(Image, FitWithinShrinksNeverGrows) {
  EXPECT_EQ(zim::fitWithin({800, 600}, 400, 400).width, 400);
  EXPECT_EQ(zim::fitWithin({800, 600}, 400, 400).height, 300);
  EXPECT_EQ(zim::fitWithin({300, 900}, 400, 450).height, 450);
  EXPECT_EQ(zim::fitWithin({300, 900}, 400, 450).width, 150);
  EXPECT_EQ(zim::fitWithin({100, 50}, 400, 400).width, 100);
}

TEST(Image, EveryWebpInTheSampleBecomesAPng) {
  zim::Archive archive;
  ASSERT_EQ(archive.open(zim::openArchiveSource(std::string(ZIM_TEST_DATA_DIR) +
                                                "/nons/wikipedia_en_climate_change_mini_2024-06.zim")),
            zim::Error::None);
  int converted = 0;
  for (uint32_t i = 0; i < archive.entryCount(); i++) {
    zim::Entry e;
    ASSERT_EQ(archive.entryAt(i, e), zim::Error::None);
    if (!e.isContent() || archive.mimeType(e.mime) != "image/webp") continue;
    std::string bytes;
    ASSERT_EQ(archive.read(e, bytes), zim::Error::None);
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    zim::ImageSize own;
    ASSERT_TRUE(zim::webpSize(data, bytes.size(), own)) << e.path;
    std::vector<uint8_t> png;
    zim::ImageSize written;
    ASSERT_TRUE(zim::webpToGrayPng(data, bytes.size(), 200, 150, png, &written)) << e.path;
    const zim::ImageSize want = zim::fitWithin(own, 200, 150);
    EXPECT_EQ(written.width, want.width) << e.path;
    EXPECT_EQ(written.height, want.height) << e.path;
    int w = 0, h = 0;
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readGrayPng(png, w, h, pixels)) << e.path;
    EXPECT_EQ(w, written.width);
    EXPECT_EQ(h, written.height);
    converted++;
  }
  EXPECT_GT(converted, 50);
}

TEST(Image, RejectsWhatIsNotWebp) {
  const uint8_t junk[64] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  zim::ImageSize s;
  EXPECT_FALSE(zim::webpSize(junk, sizeof(junk), s));
  std::vector<uint8_t> png;
  EXPECT_FALSE(zim::webpToGrayPng(junk, sizeof(junk), 100, 100, png));
  EXPECT_TRUE(png.empty());
}

}  // namespace
