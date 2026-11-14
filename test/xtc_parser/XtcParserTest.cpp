#include <HalStorage.h>
#include <XtcParser.h>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <vector>

using namespace xtc;

namespace {

constexpr const char* kPath = "/book.xtc";
constexpr uint16_t kSide = 8;
constexpr uint8_t kFill = 0x55;

// 1-bit pages are row-major ((w+7)/8 * h); 2-bit pages are two column-major planes (w * ceil(h/8) * 2).
uint32_t payloadSize(uint32_t pageMagic) { return pageMagic == XTH_MAGIC ? kSide * ((kSide + 7) / 8) * 2 : 8; }

// One-page book: header, one page table entry, one page.
std::vector<uint8_t> buildBook(uint32_t containerMagic, uint32_t pageMagic) {
  XtcHeader header{};
  header.magic = containerMagic;
  header.versionMajor = 1;
  header.pageCount = 1;
  header.pageTableOffset = sizeof(XtcHeader);
  header.dataOffset = sizeof(XtcHeader) + sizeof(PageTableEntry);

  XtgPageHeader page{};
  page.magic = pageMagic;
  page.width = kSide;
  page.height = kSide;
  page.dataSize = payloadSize(pageMagic);

  PageTableEntry entry{header.dataOffset, static_cast<uint32_t>(sizeof(XtgPageHeader) + page.dataSize), kSide, kSide};

  std::vector<uint8_t> bytes(header.dataOffset + sizeof(XtgPageHeader) + page.dataSize, kFill);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.pageTableOffset, &entry, sizeof(entry));
  std::memcpy(bytes.data() + header.dataOffset, &page, sizeof(page));
  return bytes;
}

PageTableEntry* firstEntry(std::vector<uint8_t>& bytes) {
  return reinterpret_cast<PageTableEntry*>(bytes.data() + sizeof(XtcHeader));
}

struct DepthCase {
  const char* name;
  uint32_t containerMagic;
  uint32_t pageMagic;
  uint8_t expectedDepth;
};

class XtcParserDepth : public ::testing::TestWithParam<DepthCase> {};

// Matching files keep working, and mismatched ones (xtctool writes "XTC\0" around
// "XTH\0" pages) follow the bit depth of their pages.
TEST_P(XtcParserDepth, BitDepthFollowsPagesAndPageLoads) {
  const DepthCase& c = GetParam();
  Storage.setFile(kPath, buildBook(c.containerMagic, c.pageMagic));

  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), c.expectedDepth);

  PageInfo info{};
  ASSERT_TRUE(parser.getPageInfo(0, info));
  EXPECT_EQ(info.bitDepth, c.expectedDepth);

  const size_t expected = payloadSize(c.pageMagic);
  uint8_t buffer[64]{};
  ASSERT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), expected);
  for (size_t i = 0; i < expected; i++) EXPECT_EQ(buffer[i], kFill) << "byte " << i;

  EXPECT_EQ(parser.loadPage(0, buffer, expected - 1), 0u);
  EXPECT_EQ(parser.getLastError(), XtcError::MEMORY_ERROR);

  size_t streamed = 0;
  const XtcError status = parser.loadPageStreaming(
      0,
      [&](const uint8_t* data, size_t size, size_t offset) {
        EXPECT_EQ(offset, streamed);
        for (size_t i = 0; i < size; i++) EXPECT_EQ(data[i], kFill);
        streamed += size;
      },
      3);
  EXPECT_EQ(status, XtcError::OK);
  EXPECT_EQ(streamed, expected);
}

INSTANTIATE_TEST_SUITE_P(Containers, XtcParserDepth,
                         ::testing::Values(DepthCase{"XtcWithXtgPages", XTC_MAGIC, XTG_MAGIC, 1},
                                           DepthCase{"XtchWithXthPages", XTCH_MAGIC, XTH_MAGIC, 2},
                                           DepthCase{"XtcWithXthPages", XTC_MAGIC, XTH_MAGIC, 2},
                                           DepthCase{"XtchWithXtgPages", XTCH_MAGIC, XTG_MAGIC, 1}),
                         [](const ::testing::TestParamInfo<DepthCase>& info) { return info.param.name; });

// An unknown first-page magic does not override the container; the page is still rejected.
TEST(XtcParser, UnknownPageMagicKeepsContainerDepth) {
  for (const uint32_t container : {XTC_MAGIC, XTCH_MAGIC}) {
    Storage.setFile(kPath, buildBook(container, 0x12345678));
    XtcParser parser;
    ASSERT_EQ(parser.open(kPath), XtcError::OK);
    EXPECT_EQ(parser.getBitDepth(), container == XTCH_MAGIC ? 2 : 1);
    uint8_t buffer[64]{};
    EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
    EXPECT_EQ(parser.getLastError(), XtcError::INVALID_MAGIC);
  }
}

// A first-page offset beyond the file (including one that would overflow offset + 4)
// is ignored by the probe: open still succeeds with the container depth, the page fails.
TEST(XtcParser, OutOfRangeFirstPageOffsetKeepsContainerDepth) {
  for (const uint64_t offset : {uint64_t{1} << 40, std::numeric_limits<uint64_t>::max() - 1}) {
    auto bytes = buildBook(XTC_MAGIC, XTH_MAGIC);
    firstEntry(bytes)->dataOffset = offset;
    Storage.setFile(kPath, std::move(bytes));
    XtcParser parser;
    ASSERT_EQ(parser.open(kPath), XtcError::OK);
    EXPECT_EQ(parser.getBitDepth(), 1);
    uint8_t buffer[64]{};
    EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
  }
}

// A first page cut off inside its magic is ignored the same way.
TEST(XtcParser, TruncatedFirstPageKeepsContainerDepth) {
  auto bytes = buildBook(XTC_MAGIC, XTH_MAGIC);
  bytes.resize(sizeof(XtcHeader) + sizeof(PageTableEntry) + 2);
  Storage.setFile(kPath, std::move(bytes));
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), 1);
  uint8_t buffer[64]{};
  EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
}

}  // namespace
