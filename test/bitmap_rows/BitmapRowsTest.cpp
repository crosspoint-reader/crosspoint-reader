// The 1-bit fast path of GfxRenderer::drawBitmap1Bit reads rows in batches and expands them
// through a per-image table instead of readNextRow()'s per-pixel conversion. Drawing must not
// change by a single pixel, so this holds the two to the same bytes on every shape of 1-bit BMP
// the card can hold: widths that do and do not fill whole bytes, both row orders, and palettes
// that are not plain black and white.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "Bitmap.h"

namespace {

void putLE16(std::vector<uint8_t>& out, const uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void putLE32(std::vector<uint8_t>& out, const uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// A 1-bit BMP with a two-entry palette (B, G, R, 0 each), random pixels, rows padded to 4 bytes.
// `dibSize` 108 or 124 writes a V4/V5 header, as GIMP and ImageMagick do: the 40 bytes of the
// original header, then masks and a colour-space tag that must not be read as the palette.
std::string writeOneBitBmp(const std::string& name, const int width, const int height, const bool topDown,
                           const uint8_t palette0, const uint8_t palette1, const uint32_t seed,
                           const uint32_t dibSize = 40) {
  const int rowBytes = ((width + 31) / 32) * 4;
  const uint32_t pixelOffset = 14 + dibSize + 8;
  std::vector<uint8_t> out;
  out.push_back('B');
  out.push_back('M');
  putLE32(out, pixelOffset + static_cast<uint32_t>(rowBytes * height));
  putLE32(out, 0);
  putLE32(out, pixelOffset);
  putLE32(out, dibSize);
  putLE32(out, static_cast<uint32_t>(width));
  putLE32(out, static_cast<uint32_t>(topDown ? -height : height));
  putLE16(out, 1);
  putLE16(out, 1);
  putLE32(out, 0);
  putLE32(out, static_cast<uint32_t>(rowBytes * height));
  putLE32(out, 2835);
  putLE32(out, 2835);
  putLE32(out, 2);
  putLE32(out, 2);
  if (dibSize > 40) {
    for (const uint32_t mask : {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u}) putLE32(out, mask);
    putLE32(out, 0x73524742);  // 'sRGB'
    while (out.size() < 14 + dibSize) out.push_back(0x7F);
  }
  for (const uint8_t gray : {palette0, palette1}) {
    out.push_back(gray);
    out.push_back(gray);
    out.push_back(gray);
    out.push_back(0);
  }
  std::mt19937 rng(seed);
  for (int i = 0; i < rowBytes * height; ++i) out.push_back(static_cast<uint8_t>(rng()));

  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return path;
}

// Every row both ways, compared over the bytes readNextRow() writes.
void expectSameRows(const std::string& path, const int batchRows) {
  FsFile a;
  FsFile b;
  ASSERT_TRUE(Storage.openFileForRead("T", path, a));
  ASSERT_TRUE(Storage.openFileForRead("T", path, b));
  Bitmap perPixel(a);
  Bitmap batched(b);
  ASSERT_EQ(perPixel.parseHeaders(), BmpReaderError::Ok);
  ASSERT_EQ(batched.parseHeaders(), BmpReaderError::Ok);
  ASSERT_TRUE(batched.is1Bit());

  const int width = batched.getWidth();
  const int height = batched.getHeight();
  const int rowBytes = batched.getRowBytes();
  const int packed = (width + 3) / 4;
  std::vector<uint8_t> expected(2 * ((width + 7) / 8), 0);
  std::vector<uint8_t> scratch(rowBytes);
  std::vector<uint8_t> actual(2 * ((width + 7) / 8), 0);
  std::vector<uint8_t> raw(static_cast<size_t>(batchRows) * rowBytes);
  uint16_t table[256];
  batched.oneBitExpansion(table);

  for (int y = 0; y < height; y += batchRows) {
    const int rows = std::min(batchRows, height - y);
    ASSERT_EQ(batched.readRawRows(raw.data(), rows), BmpReaderError::Ok);
    for (int r = 0; r < rows; ++r) {
      ASSERT_EQ(perPixel.readNextRow(expected.data(), scratch.data()), BmpReaderError::Ok);
      Bitmap::expandOneBitRow(table, raw.data() + static_cast<size_t>(r) * rowBytes, actual.data(), width);
      for (int i = 0; i < packed; ++i) {
        ASSERT_EQ(actual[i], expected[i]) << path << " row " << (y + r) << " byte " << i;
      }
    }
  }
}

}  // namespace

TEST(BitmapRows, OneBitExpansionMatchesReadNextRowAtEveryWidth) {
  for (int width = 1; width <= 40; ++width) {
    const auto path = writeOneBitBmp("bmp_rows_w" + std::to_string(width) + ".bmp", width, 7, false, 0, 255, width);
    expectSameRows(path, 3);
  }
}

TEST(BitmapRows, OneBitExpansionMatchesForGridSizedCovers) {
  for (const auto& [w, h] : {std::pair{160, 240}, std::pair{196, 196}, std::pair{154, 210}, std::pair{340, 540}}) {
    const auto path = writeOneBitBmp("bmp_rows_cover.bmp", w, h, false, 0, 255, static_cast<uint32_t>(w * h));
    expectSameRows(path, 2048 / (((w + 31) / 32) * 4));  // the renderer's batch
  }
}

TEST(BitmapRows, OneBitExpansionMatchesTopDownRows) {
  const auto path = writeOneBitBmp("bmp_rows_topdown.bmp", 37, 11, true, 0, 255, 99);
  expectSameRows(path, 4);
}

// The table comes from the palette, not from an assumption that index 0 is black.
TEST(BitmapRows, OneBitExpansionMatchesInvertedAndGrayPalettes) {
  expectSameRows(writeOneBitBmp("bmp_rows_inverted.bmp", 29, 9, false, 255, 0, 5), 2);
  expectSameRows(writeOneBitBmp("bmp_rows_gray.bmp", 29, 9, false, 90, 180, 6), 5);
  expectSameRows(writeOneBitBmp("bmp_rows_same.bmp", 29, 9, false, 128, 128, 7), 9);
}

TEST(BitmapRows, BatchOfOneMatchesToo) {
  expectSameRows(writeOneBitBmp("bmp_rows_one.bmp", 17, 5, false, 0, 255, 8), 1);
}

TEST(BitmapRows, ReadingPastTheLastRowFails) {
  const auto path = writeOneBitBmp("bmp_rows_short.bmp", 16, 4, false, 0, 255, 9);
  FsFile f;
  ASSERT_TRUE(Storage.openFileForRead("T", path, f));
  Bitmap bmp(f);
  ASSERT_EQ(bmp.parseHeaders(), BmpReaderError::Ok);
  std::vector<uint8_t> raw(static_cast<size_t>(5) * bmp.getRowBytes());
  EXPECT_EQ(bmp.readRawRows(raw.data(), 5), BmpReaderError::ShortReadRow);
}

// The palette is read after the whole DIB header. With a V4 or V5 header it used to be read from
// byte 54, inside the masks, so the black and white of a GIMP export came out as two greys.
TEST(BitmapRows, ExtendedHeadersReadThePaletteAfterTheHeader) {
  for (const uint32_t dibSize : {108u, 124u}) {
    const auto plainPath = writeOneBitBmp("bmp_hdr_40.bmp", 21, 3, false, 0, 255, 11);
    const auto extPath =
        writeOneBitBmp("bmp_hdr_" + std::to_string(dibSize) + ".bmp", 21, 3, false, 0, 255, 11, dibSize);
    FsFile a;
    FsFile b;
    ASSERT_TRUE(Storage.openFileForRead("T", plainPath, a));
    ASSERT_TRUE(Storage.openFileForRead("T", extPath, b));
    Bitmap plain(a);
    Bitmap extended(b);
    ASSERT_EQ(plain.parseHeaders(), BmpReaderError::Ok);
    ASSERT_EQ(extended.parseHeaders(), BmpReaderError::Ok) << dibSize;
    uint16_t want[256];
    uint16_t got[256];
    plain.oneBitExpansion(want);
    extended.oneBitExpansion(got);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], want[i]) << dibSize << " byte " << i;
    expectSameRows(extPath, 2);
  }
}

// A file cut off inside its palette is refused rather than drawn with whatever was read.
TEST(BitmapRows, PaletteCutShortIsRefused) {
  const auto path = writeOneBitBmp("bmp_hdr_cut_src.bmp", 16, 2, false, 0, 255, 12);
  std::vector<uint8_t> bytes(14 + 40 + 6);
  FILE* in = std::fopen(path.c_str(), "rb");
  ASSERT_EQ(std::fread(bytes.data(), 1, bytes.size(), in), bytes.size());
  std::fclose(in);
  const std::string cut = (std::filesystem::temp_directory_path() / "bmp_hdr_cut.bmp").string();
  FILE* out = std::fopen(cut.c_str(), "wb");
  std::fwrite(bytes.data(), 1, bytes.size(), out);
  std::fclose(out);

  FsFile f;
  ASSERT_TRUE(Storage.openFileForRead("T", cut, f));
  Bitmap bmp(f);
  EXPECT_EQ(bmp.parseHeaders(), BmpReaderError::FileInvalid);
}
