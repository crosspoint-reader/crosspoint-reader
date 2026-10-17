// XtcParser takes the bit depth from the pages, not from the container header: xtctool writes a
// 1-bit "XTC" header around 2-bit "XTH" pages, and a parser that trusted the header rejected every
// page of such a book ("Page load error").

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "XtcParser.h"

namespace {

using xtc::XtcError;
using xtc::XtcParser;

constexpr uint16_t kWidth = 16;
constexpr uint16_t kHeight = 8;
constexpr uint16_t kPages = 2;

void putLE(std::vector<uint8_t>& out, const uint64_t v, const int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

size_t bitmapBytes(const uint32_t pageMagic) {
  return pageMagic == xtc::XTH_MAGIC ? ((kWidth * kHeight + 7) / 8) * 2 : ((kWidth + 7) / 8) * kHeight;
}

uint8_t bitmapByte(const uint32_t page, const size_t i) { return static_cast<uint8_t>(page * 31 + i); }

// A minimal book: 56-byte header, the page table right after it, then `kPages` pages that all
// carry `pageMagic`, whatever the container magic says.
std::string writeBook(const std::string& name, const uint32_t containerMagic, const uint32_t pageMagic) {
  constexpr uint64_t kHeaderSize = 56;
  constexpr uint64_t kTableSize = kPages * 16;
  const size_t bitmapSize = bitmapBytes(pageMagic);
  const uint64_t pageSize = 22 + bitmapSize;

  std::vector<uint8_t> out;
  putLE(out, containerMagic, 4);
  out.push_back(1);  // version 1.0
  out.push_back(0);
  putLE(out, kPages, 2);
  out.push_back(0);                         // readDirection
  out.push_back(0);                         // hasMetadata
  out.push_back(0);                         // hasThumbnails
  out.push_back(0);                         // hasChapters
  putLE(out, 0, 4);                         // currentPage
  putLE(out, 0, 8);                         // metadataOffset
  putLE(out, kHeaderSize, 8);               // pageTableOffset
  putLE(out, kHeaderSize + kTableSize, 8);  // dataOffset
  putLE(out, 0, 8);                         // thumbOffset
  putLE(out, 0, 4);                         // chapterOffset
  putLE(out, 0, 4);                         // padding

  for (uint32_t p = 0; p < kPages; ++p) {
    putLE(out, kHeaderSize + kTableSize + p * pageSize, 8);
    putLE(out, pageSize, 4);
    putLE(out, kWidth, 2);
    putLE(out, kHeight, 2);
  }
  for (uint32_t p = 0; p < kPages; ++p) {
    putLE(out, pageMagic, 4);
    putLE(out, kWidth, 2);
    putLE(out, kHeight, 2);
    out.push_back(0);  // colorMode
    out.push_back(0);  // compression
    putLE(out, bitmapSize, 4);
    putLE(out, 0, 8);  // md5
    for (size_t i = 0; i < bitmapSize; ++i) out.push_back(bitmapByte(p, i));
  }

  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  if (f) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
  return path;
}

struct Case {
  const char* name;
  uint32_t containerMagic;
  uint32_t pageMagic;
  uint8_t expectedDepth;
};

void PrintTo(const Case& c, std::ostream* os) { *os << c.name; }

class XtcBitDepthTest : public ::testing::TestWithParam<Case> {};

TEST_P(XtcBitDepthTest, DepthFollowsThePagesAndEveryPageLoads) {
  const Case& c = GetParam();
  const std::string path = writeBook(std::string("xtc_bit_depth_") + c.name + ".xtc", c.containerMagic, c.pageMagic);

  XtcParser parser;
  ASSERT_EQ(parser.open(path.c_str()), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), c.expectedDepth);

  std::vector<uint8_t> buffer(bitmapBytes(c.pageMagic));
  for (uint32_t p = 0; p < kPages; ++p) {
    EXPECT_EQ(parser.loadPage(p, buffer.data(), buffer.size()), buffer.size()) << "page " << p;
    EXPECT_EQ(parser.getLastError(), XtcError::OK) << "page " << p;
    EXPECT_EQ(buffer[1], bitmapByte(p, 1)) << "page " << p;

    uint8_t range[4] = {};
    EXPECT_EQ(parser.loadPageRange(p, 1, range, sizeof(range)), sizeof(range)) << "page " << p;
    EXPECT_EQ(range[0], bitmapByte(p, 1)) << "page " << p;
    parser.endPageRange();
  }
  parser.close();
  std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(Containers, XtcBitDepthTest,
                         ::testing::Values(Case{"xtc_xtg", xtc::XTC_MAGIC, xtc::XTG_MAGIC, 1},
                                           Case{"xtch_xth", xtc::XTCH_MAGIC, xtc::XTH_MAGIC, 2},
                                           // xtctool: 1-bit container magic around 2-bit pages.
                                           Case{"xtc_xth", xtc::XTC_MAGIC, xtc::XTH_MAGIC, 2},
                                           Case{"xtch_xtg", xtc::XTCH_MAGIC, xtc::XTG_MAGIC, 1}),
                         [](const ::testing::TestParamInfo<Case>& info) { return std::string(info.param.name); });

// A first page whose magic is neither XTG nor XTH leaves the container's depth in place.
TEST(XtcBitDepth, UnknownPageMagicKeepsTheContainerDepth) {
  const std::string path = writeBook("xtc_bit_depth_unknown.xtc", xtc::XTCH_MAGIC, 0x00585858);
  XtcParser parser;
  ASSERT_EQ(parser.open(path.c_str()), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), 2);
  parser.close();
  std::filesystem::remove(path);
}

}  // namespace
