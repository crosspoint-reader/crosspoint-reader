// PngStreamDecoder::begin must reject an IHDR whose bit depth the colour type does not allow
// (PNG spec table 11.1). nextRow() unpacks rows by colour type and depth; a pairing outside the
// table either indexes past the row buffer (grayscale/palette depth 3/5/6/7) or divides by
// ppb = 8 / bitDepth == 0 (grayscale 9-15, palette 16). The decoder never checked, so a corrupt
// image in an EPUB got past begin() and faulted (or read garbage) when its rows were decoded.
//
// The PNGs are built in memory: IDAT carries one stored (uncompressed) deflate block so every
// row's bytes are explicit. CRCs are left zero because the decoder skips them.
#include <HalStorage.h>
#include <PngStreamDecoder.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void appendBE32(std::vector<uint8_t>& out, const uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void appendChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  appendBE32(out, static_cast<uint32_t>(data.size()));
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  appendBE32(out, 0);  // CRC: not verified by the decoder
}

// zlib stream holding `raw` in one stored deflate block (raw.size() <= 65535).
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> z = {0x78, 0x01, 0x01};  // CMF, FLG, BFINAL=1 + BTYPE=00 (stored)
  const auto len = static_cast<uint16_t>(raw.size());
  z.push_back(static_cast<uint8_t>(len));
  z.push_back(static_cast<uint8_t>(len >> 8));
  z.push_back(static_cast<uint8_t>(~len));
  z.push_back(static_cast<uint8_t>(~len >> 8));
  z.insert(z.end(), raw.begin(), raw.end());
  uint32_t a = 1, b = 0;
  for (const uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  appendBE32(z, (b << 16) | a);
  return z;
}

int channelsFor(const uint8_t colorType) {
  switch (colorType) {
    case 2:
      return 3;  // RGB
    case 4:
      return 2;  // grayscale + alpha
    case 6:
      return 4;  // RGBA
    default:
      return 1;  // grayscale, palette
  }
}

// A width x height PNG with filter-None rows of a repeating byte pattern. The row length follows
// the spec formula for the declared depth, so a valid header decodes cleanly to the last row.
// Shapes too large for one stored block get a token IDAT: begin() has to refuse them first.
std::vector<uint8_t> buildPng(const uint32_t width, const uint32_t height, const uint8_t bitDepth,
                              const uint8_t colorType) {
  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};

  std::vector<uint8_t> ihdr;
  appendBE32(ihdr, width);
  appendBE32(ihdr, height);
  ihdr.insert(ihdr.end(), {bitDepth, colorType, 0, 0, 0});  // compression, filter, interlace
  appendChunk(png, "IHDR", ihdr);

  if (colorType == 3) {
    std::vector<uint8_t> plte;
    for (int i = 0; i < 4; ++i) plte.insert(plte.end(), {static_cast<uint8_t>(i * 80), 0x40, 0x80});
    appendChunk(png, "PLTE", plte);
  }

  const uint64_t rowBytes = (static_cast<uint64_t>(width) * channelsFor(colorType) * bitDepth + 7) / 8;
  const bool fits = (rowBytes + 1) * height <= 65535;
  std::vector<uint8_t> raw;
  for (uint32_t y = 0; fits && y < height; ++y) {
    raw.push_back(0);  // filter None
    for (uint64_t i = 0; i < rowBytes; ++i) raw.push_back(static_cast<uint8_t>(0x5A + i + y));
  }
  appendChunk(png, "IDAT", zlibStored(raw));
  appendChunk(png, "IEND", {});
  return png;
}

class PngDecoderIhdr : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    path_ = (std::filesystem::temp_directory_path() /
             (std::string("png-ihdr-") + info->name() + "-" + std::to_string(getpid()) + ".png"))
                .string();
  }
  void TearDown() override { std::remove(path_.c_str()); }

  // begin() result for the PNG; on success also decodes every row and reports whether all of
  // them came out.
  bool decode(const std::vector<uint8_t>& png, bool* allRows = nullptr) {
    FILE* fp = std::fopen(path_.c_str(), "wb");
    if (!fp) return false;
    std::fwrite(png.data(), 1, png.size(), fp);
    std::fclose(fp);

    FsFile file;
    if (!Storage.openFileForRead("TST", path_, file)) return false;
    PngStreamDecoder decoder;
    PngStreamDecoder::Info info;
    const bool started = decoder.begin(file, info);
    if (started && allRows) {
      std::vector<uint8_t> gray(info.width), alpha(info.width);
      *allRows = true;
      for (uint32_t y = 0; y < info.height; ++y) {
        if (!decoder.nextRow(gray.data(), alpha.data())) *allRows = false;
      }
      *allRows = *allRows && !decoder.nextRow(gray.data());  // nothing past the last row
    }
    decoder.end();
    file.close();
    return started;
  }

  std::string path_;
};

struct DepthCombo {
  uint8_t colorType;
  uint8_t bitDepth;
};

// PNG spec table 11.1.
constexpr DepthCombo kValidCombos[] = {
    {0, 1}, {0, 2},  {0, 4}, {0, 8}, {0, 16},  // grayscale
    {2, 8}, {2, 16},                           // RGB
    {3, 1}, {3, 2},  {3, 4}, {3, 8},           // palette
    {4, 8}, {4, 16},                           // grayscale + alpha
    {6, 8}, {6, 16},                           // RGBA
};

bool isValidCombo(const uint8_t colorType, const uint8_t bitDepth) {
  for (const auto& c : kValidCombos) {
    if (c.colorType == colorType && c.bitDepth == bitDepth) return true;
  }
  return false;
}

TEST_F(PngDecoderIhdr, EverySpecDepthStillDecodes) {
  // Odd width so the sub-byte depths end on a partial byte.
  for (const auto& c : kValidCombos) {
    bool allRows = false;
    EXPECT_TRUE(decode(buildPng(5, 3, c.bitDepth, c.colorType), &allRows))
        << "colorType " << int(c.colorType) << " depth " << int(c.bitDepth);
    EXPECT_TRUE(allRows) << "colorType " << int(c.colorType) << " depth " << int(c.bitDepth);
  }
}

TEST_F(PngDecoderIhdr, RejectsDepthTheColourTypeDoesNotAllow) {
  // Only begin() runs here: before the fix these headers were accepted, and decoding their rows
  // would read past the row buffer or divide by zero.
  for (const uint8_t colorType : {0, 2, 3, 4, 6}) {
    for (int depth = 0; depth <= 32; ++depth) {
      if (isValidCombo(colorType, static_cast<uint8_t>(depth))) continue;
      EXPECT_FALSE(decode(buildPng(5, 3, static_cast<uint8_t>(depth), colorType)))
          << "colorType " << int(colorType) << " depth " << depth;
    }
    EXPECT_FALSE(decode(buildPng(5, 3, 255, colorType))) << "colorType " << int(colorType) << " depth 255";
  }
}

TEST_F(PngDecoderIhdr, RejectsTheCrashingHeaders) {
  // The shapes from the report, named so a regression points straight at them.
  EXPECT_FALSE(decode(buildPng(64, 2, 3, 0))) << "grayscale depth 3: row read past its buffer";
  EXPECT_FALSE(decode(buildPng(64, 2, 9, 0))) << "grayscale depth 9: ppb = 8 / 9 = 0";
  EXPECT_FALSE(decode(buildPng(64, 2, 16, 3))) << "palette depth 16: ppb = 8 / 16 = 0";
}

TEST_F(PngDecoderIhdr, RejectsOtherCorruptHeaderFields) {
  EXPECT_FALSE(decode(buildPng(0, 3, 8, 0))) << "zero width";
  EXPECT_FALSE(decode(buildPng(5, 0, 8, 0))) << "zero height";
  EXPECT_FALSE(decode(buildPng(0xFFFFFFFFu, 3, 8, 0))) << "huge width";
  EXPECT_FALSE(decode(buildPng(5, 0xFFFFFFFFu, 8, 0))) << "huge height";
  EXPECT_FALSE(decode(buildPng(5, 3, 8, 1))) << "colour type 1";
  EXPECT_FALSE(decode(buildPng(5, 3, 8, 7))) << "colour type 7";

  // Compression, filter and interlace bytes: IHDR data offsets 10, 11, 12.
  for (const size_t field : {10u, 11u, 12u}) {
    auto png = buildPng(5, 3, 8, 0);
    png[8 + 8 + field] = 1;  // signature, chunk length + type, then IHDR data
    EXPECT_FALSE(decode(png)) << "IHDR byte " << field;
  }
}

}  // namespace
