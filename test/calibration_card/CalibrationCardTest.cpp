#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "CalibrationCard.h"

namespace {
struct Buf final : calibcard::Sink {
  std::vector<uint8_t> v;
  void put(const uint8_t* d, size_t n) override { v.insert(v.end(), d, d + n); }
};
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
}  // namespace

// The layout of the KEINKT Calibrator page (calibrator/index.html, renderCard).
TEST(CalibrationCard, LayoutMatchesThePage) {
  using calibcard::pixel;
  EXPECT_EQ(pixel(40, 40, false), 255);  // mark hole centre
  EXPECT_EQ(pixel(14, 14, false), 0);    // mark ring
  EXPECT_EQ(pixel(440, 760, false), 255);
  EXPECT_EQ(pixel(36 + 36, 112 + 36, false), 0);                                // level 0
  EXPECT_EQ(pixel(36 + 2 * 84 + 36, 112 + 3 * 84 + 36, false), 135);            // level 17: round(17·255/32 = 135.47)
  EXPECT_EQ(pixel(36 + 16 % 5 * 84 + 36, 112 + 16 / 5 * 84 + 36, false), 128);  // level 16: 127.5 → 128, as Math.round
  EXPECT_EQ(pixel(36 + 4 * 84 + 36, 112 + 6 * 84 + 36, false), 255);            // last cell: paper on the pictures card
  EXPECT_EQ(pixel(36 + 4 * 84 + 36, 112 + 6 * 84 + 36, true), 0);               // black on the books card
  EXPECT_EQ(pixel(36 + 72 + 6, 112 + 36, false), 255);                          // the gap between cells
}

TEST(CalibrationCard, Crc32CheckValue) {
  const char* s = "123456789";
  EXPECT_EQ(calibcard::crc32(0, reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(CalibrationCard, PngDecodesToTheCard) {
  Buf b;
  calibcard::writePng(b, true);
  const auto& v = b.v;
  ASSERT_GT(v.size(), 8u);
  ASSERT_EQ(memcmp(v.data(), "\x89PNG\r\n\x1a\n", 8), 0);
  size_t pos = 8;
  std::vector<uint8_t> idat;
  bool sawEnd = false;
  while (pos + 12 <= v.size()) {
    const uint32_t len = be32(&v[pos]);
    const std::string type(reinterpret_cast<const char*>(&v[pos + 4]), 4);
    ASSERT_LE(pos + 12 + len, v.size());
    EXPECT_EQ(calibcard::crc32(0, &v[pos + 4], len + 4), be32(&v[pos + 8 + len])) << type;
    if (type == "IHDR") {
      EXPECT_EQ(be32(&v[pos + 8]), uint32_t(calibcard::W));
      EXPECT_EQ(be32(&v[pos + 12]), uint32_t(calibcard::H));
    }
    if (type == "IDAT") idat.insert(idat.end(), &v[pos + 8], &v[pos + 8 + len]);
    if (type == "IEND") sawEnd = true;
    pos += 12 + len;
  }
  ASSERT_TRUE(sawEnd);
  // zlib: header, stored blocks, Adler-32.
  ASSERT_GE(idat.size(), 6u);
  size_t p = 2;
  std::vector<uint8_t> raw;
  bool last = false;
  while (!last) {
    last = idat[p] & 1;
    ASSERT_EQ(idat[p] & 6, 0);  // stored
    const uint16_t len = le16(&idat[p + 1]), nlen = le16(&idat[p + 3]);
    ASSERT_EQ(uint16_t(~len), nlen);
    raw.insert(raw.end(), &idat[p + 5], &idat[p + 5 + len]);
    p += 5 + len;
  }
  uint32_t a = 1, s = 0;
  for (const uint8_t c : raw) {
    a = (a + c) % 65521;
    s = (s + a) % 65521;
  }
  EXPECT_EQ(be32(&idat[p]), (s << 16) | a);
  ASSERT_EQ(raw.size(), size_t(calibcard::W + 1) * calibcard::H);
  for (int y = 0; y < calibcard::H; ++y) {
    ASSERT_EQ(raw[y * (calibcard::W + 1)], 0);
    for (int x = 0; x < calibcard::W; ++x) ASSERT_EQ(raw[y * (calibcard::W + 1) + 1 + x], calibcard::pixel(x, y, true));
  }
}

TEST(CalibrationCard, EpubIsAValidStoreZip) {
  Buf b;
  calibcard::writeEpub(b, "Books & <cards>");
  const auto& v = b.v;
  // End of central directory.
  ASSERT_GE(v.size(), 22u);
  const size_t eocd = v.size() - 22;
  ASSERT_EQ(le32(&v[eocd]), 0x06054b50u);
  const uint16_t n = le16(&v[eocd + 10]);
  const uint32_t cdSize = le32(&v[eocd + 12]), cdStart = le32(&v[eocd + 16]);
  EXPECT_EQ(n, 6);
  EXPECT_EQ(cdStart + cdSize, eocd);
  // mimetype first, stored, exact content (EPUB OCF).
  ASSERT_EQ(le32(&v[0]), 0x04034b50u);
  EXPECT_EQ(le16(&v[8]), 0);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(&v[30]), 8), "mimetype");
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(&v[38]), 20), "application/epub+zip");
  size_t c = cdStart;
  bool sawPng = false, sawOpf = false;
  for (int i = 0; i < n; ++i) {
    ASSERT_EQ(le32(&v[c]), 0x02014b50u);
    const uint32_t crc = le32(&v[c + 16]), size = le32(&v[c + 20]);
    const uint16_t nameLen = le16(&v[c + 28]);
    const uint32_t off = le32(&v[c + 42]);
    const std::string name(reinterpret_cast<const char*>(&v[c + 46]), nameLen);
    ASSERT_EQ(le32(&v[off]), 0x04034b50u) << name;
    EXPECT_EQ(le32(&v[off + 14]), crc) << name;
    const size_t data = off + 30 + le16(&v[off + 26]) + le16(&v[off + 28]);
    ASSERT_LE(data + size, v.size());
    EXPECT_EQ(calibcard::crc32(0, &v[data], size), crc) << name;
    if (name == "OEBPS/card.png") sawPng = true;
    if (name == "OEBPS/content.opf") {
      sawOpf = true;
      const std::string opf(reinterpret_cast<const char*>(&v[data]), size);
      EXPECT_NE(opf.find("<dc:title>Books &amp; &lt;cards&gt;</dc:title>"), std::string::npos);
    }
    c += 46 + nameLen + le16(&v[c + 30]) + le16(&v[c + 32]);
  }
  EXPECT_TRUE(sawPng);
  EXPECT_TRUE(sawOpf);
}
