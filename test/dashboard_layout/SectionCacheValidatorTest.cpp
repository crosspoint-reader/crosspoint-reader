#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Epub/Epub/SectionCacheValidator.h"

namespace {
constexpr uint8_t FINAL_VERSION = 31;
constexpr uint8_t PARTIAL_VERSION = 0xFB;
constexpr uint64_t HEADER_SIZE = 39;
constexpr size_t PAGE_COUNT_OFFSET = 21;
constexpr size_t PAGE_LUT_OFFSET = 23;
constexpr size_t ANCHOR_MAP_OFFSET = 27;
constexpr size_t PARAGRAPH_LUT_OFFSET = 31;
constexpr size_t LIST_ITEM_LUT_OFFSET = 35;

template <typename T>
void append(std::vector<uint8_t>& bytes, const T& value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
void writeAt(std::vector<uint8_t>& bytes, const size_t offset, const T& value) {
  ASSERT_LE(offset + sizeof(T), bytes.size());
  memcpy(bytes.data() + offset, &value, sizeof(T));
}

class MemoryInput final : public SectionCacheValidation::Input {
 public:
  explicit MemoryInput(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  uint64_t size() const override { return bytes_.size(); }
  bool readAt(const uint64_t offset, void* destination, const size_t length) override {
    if ((!destination && length != 0) || offset > bytes_.size() || length > bytes_.size() - offset) return false;
    if (length != 0) memcpy(destination, bytes_.data() + offset, length);
    return true;
  }

 private:
  const std::vector<uint8_t>& bytes_;
};

std::vector<uint8_t> emptyPage() {
  std::vector<uint8_t> page;
  append<uint16_t>(page, 0);  // elements
  append<uint16_t>(page, 0);  // footnotes
  return page;
}

std::vector<uint8_t> imagePage() {
  std::vector<uint8_t> page;
  append<uint16_t>(page, 1);
  append<uint8_t>(page, 2);
  append<int16_t>(page, 0);
  append<int16_t>(page, 0);
  append<uint32_t>(page, 3);
  page.insert(page.end(), {'i', 'm', 'g'});
  append<int16_t>(page, 10);
  append<int16_t>(page, 12);
  append<uint16_t>(page, 0);
  return page;
}

std::vector<uint8_t> textPage() {
  std::vector<uint8_t> page;
  append<uint16_t>(page, 1);
  append<uint8_t>(page, 1);
  append<int16_t>(page, 0);
  append<int16_t>(page, 0);
  append<uint16_t>(page, 1);  // words
  append<uint8_t>(page, 0);   // focus arrays absent
  append<uint16_t>(page, 2);  // "a\0"
  append<uint16_t>(page, 0);  // text offset
  append<int16_t>(page, 0);   // x
  append<uint8_t>(page, 0);   // style
  page.insert(page.end(), {'a', '\0'});
  append<uint8_t>(page, 0);  // alignment
  append<uint8_t>(page, 0);  // text-align-defined
  for (uint8_t field = 0; field < 9; ++field) append<int16_t>(page, 0);
  append<uint8_t>(page, 0);   // indent-defined
  append<uint8_t>(page, 0);   // rtl
  append<uint8_t>(page, 0);   // direction-defined
  append<uint16_t>(page, 0);  // footnotes
  return page;
}

std::vector<uint8_t> twoWordTextPage() {
  std::vector<uint8_t> page;
  append<uint16_t>(page, 1);
  append<uint8_t>(page, 1);
  append<int16_t>(page, 0);
  append<int16_t>(page, 0);
  append<uint16_t>(page, 2);  // words
  append<uint8_t>(page, 0);   // focus arrays absent
  append<uint16_t>(page, 5);  // "ab\0c\0"
  append<uint16_t>(page, 0);
  append<uint16_t>(page, 3);
  append<int16_t>(page, 0);
  append<int16_t>(page, 10);
  append<uint8_t>(page, 0);
  append<uint8_t>(page, 0);
  page.insert(page.end(), {'a', 'b', '\0', 'c', '\0'});
  append<uint8_t>(page, 0);  // alignment
  append<uint8_t>(page, 0);  // text-align-defined
  for (uint8_t field = 0; field < 9; ++field) append<int16_t>(page, 0);
  append<uint8_t>(page, 0);   // indent-defined
  append<uint8_t>(page, 0);   // rtl
  append<uint8_t>(page, 0);   // direction-defined
  append<uint16_t>(page, 0);  // footnotes
  return page;
}

struct CacheBytes {
  std::vector<uint8_t> bytes;
  uint32_t pageLut = 0;
  uint32_t anchorMap = 0;
  uint32_t paragraphLut = 0;
  uint32_t listItemLut = 0;
};

CacheBytes cacheWithPage(const std::vector<uint8_t>& page, const bool partial = false, const bool withAnchor = false) {
  CacheBytes cache;
  auto& bytes = cache.bytes;
  append<uint8_t>(bytes, partial ? PARTIAL_VERSION : FINAL_VERSION);
  append<int32_t>(bytes, 1);
  append<float>(bytes, 1.0F);
  append<uint8_t>(bytes, 0);
  append<uint8_t>(bytes, 0);
  append<uint16_t>(bytes, 600);
  append<uint16_t>(bytes, 800);
  append<uint8_t>(bytes, 1);
  append<uint8_t>(bytes, 0);
  append<uint8_t>(bytes, 0);
  append<uint8_t>(bytes, 0);
  append<uint8_t>(bytes, 0);
  append<uint8_t>(bytes, 0);
  append<uint16_t>(bytes, 1);
  for (uint8_t field = 0; field < 4; ++field) append<uint32_t>(bytes, 0);
  EXPECT_EQ(bytes.size(), HEADER_SIZE);

  bytes.insert(bytes.end(), page.begin(), page.end());
  cache.pageLut = bytes.size();
  append<uint32_t>(bytes, static_cast<uint32_t>(HEADER_SIZE));
  cache.anchorMap = bytes.size();
  append<uint16_t>(bytes, withAnchor ? 1 : 0);
  if (withAnchor) {
    append<uint32_t>(bytes, 3);
    bytes.insert(bytes.end(), {'t', 'o', 'p'});
    append<uint16_t>(bytes, 0);
  }
  cache.paragraphLut = bytes.size();
  append<uint16_t>(bytes, 1);
  append<uint16_t>(bytes, 1);
  cache.listItemLut = bytes.size();
  append<uint16_t>(bytes, 1);
  if (partial) {
    append<uint32_t>(bytes, 100);
    append<uint32_t>(bytes, 200);
  }

  writeAt<uint32_t>(bytes, PAGE_LUT_OFFSET, cache.pageLut);
  writeAt<uint32_t>(bytes, ANCHOR_MAP_OFFSET, cache.anchorMap);
  writeAt<uint32_t>(bytes, PARAGRAPH_LUT_OFFSET, cache.paragraphLut);
  writeAt<uint32_t>(bytes, LIST_ITEM_LUT_OFFSET, cache.listItemLut);
  return cache;
}

bool validates(const std::vector<uint8_t>& bytes, SectionCacheValidation::Layout* output = nullptr) {
  MemoryInput input(bytes);
  SectionCacheValidation::Layout layout;
  const bool valid = SectionCacheValidation::validate(input, HEADER_SIZE, FINAL_VERSION, PARTIAL_VERSION, layout);
  if (output) *output = layout;
  return valid;
}
}  // namespace

TEST(SectionCacheValidator, AcceptsCompleteFinalizedAndPartialFiles) {
  SectionCacheValidation::Layout layout;
  const auto finalized = cacheWithPage(textPage(), false, true);
  ASSERT_TRUE(validates(finalized.bytes, &layout));
  EXPECT_FALSE(layout.partial);
  EXPECT_EQ(layout.pageCount, 1);

  const auto partial = cacheWithPage(imagePage(), true, true);
  ASSERT_TRUE(validates(partial.bytes, &layout));
  EXPECT_TRUE(layout.partial);
  EXPECT_EQ(layout.partialBytesConsumed, 100U);
  EXPECT_EQ(layout.partialTotalBytes, 200U);
}

TEST(SectionCacheValidator, RejectsEveryTruncationOfAValidFile) {
  const auto valid = cacheWithPage(textPage(), false, true).bytes;
  for (size_t size = 0; size < valid.size(); ++size) {
    std::vector<uint8_t> truncated(valid.begin(), valid.begin() + size);
    EXPECT_FALSE(validates(truncated)) << "accepted truncated size " << size;
  }
}

TEST(SectionCacheValidator, RejectsInvalidHeaderAndTableOffsets) {
  auto cache = cacheWithPage(emptyPage());
  cache.bytes[9] = 2;  // invalid serialized bool
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  cache.bytes[19] = 3;  // invalid EPUB render mode
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  cache.bytes[20] = 2;  // invalid serialized force-indent bool
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  writeAt<uint32_t>(cache.bytes, ANCHOR_MAP_OFFSET, cache.anchorMap + 1);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  writeAt<uint32_t>(cache.bytes, cache.pageLut, 0);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  writeAt<uint16_t>(cache.bytes, cache.paragraphLut, 2);
  EXPECT_FALSE(validates(cache.bytes));
}

TEST(SectionCacheValidator, RejectsCorruptVariableLengthsAndPageMetadata) {
  auto cache = cacheWithPage(imagePage());
  constexpr size_t IMAGE_PATH_LENGTH_IN_PAGE = 2 + 1 + 2 + 2;
  writeAt<uint32_t>(cache.bytes, HEADER_SIZE + IMAGE_PATH_LENGTH_IN_PAGE, UINT32_MAX);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(textPage());
  constexpr size_t TEXT_WORD_COUNT_IN_PAGE = 2 + 1 + 2 + 2;
  writeAt<uint16_t>(cache.bytes, HEADER_SIZE + TEXT_WORD_COUNT_IN_PAGE,
                    SectionCacheValidation::MAX_TEXT_BLOCK_WORDS + 1U);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(textPage());
  const size_t textTerminator = HEADER_SIZE + textPage().size() - sizeof(uint16_t) - 23U - 1U;
  cache.bytes[textTerminator] = 'x';
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(twoWordTextPage());
  constexpr size_t SECOND_TEXT_OFFSET_IN_PAGE = 2 + 1 + 2 + 2 + 2 + 1 + 2 + 2;
  writeAt<uint16_t>(cache.bytes, HEADER_SIZE + SECOND_TEXT_OFFSET_IN_PAGE, 2);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage(), false, true);
  writeAt<uint32_t>(cache.bytes, cache.anchorMap + sizeof(uint16_t), UINT32_MAX);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage(), false, true);
  writeAt<uint32_t>(cache.bytes, cache.anchorMap + sizeof(uint16_t), SectionCacheValidation::MAX_ANCHOR_BYTES + 1U);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage(), false, true);
  const size_t anchorPage = cache.anchorMap + sizeof(uint16_t) + sizeof(uint32_t) + 3U;
  writeAt<uint16_t>(cache.bytes, anchorPage, 1);
  EXPECT_FALSE(validates(cache.bytes));
}

TEST(SectionCacheValidator, RejectsInvalidPartialWatermarkAndChangedPageLut) {
  auto cache = cacheWithPage(emptyPage(), true);
  writeAt<uint32_t>(cache.bytes, cache.bytes.size() - 8U, 0);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage(), true);
  writeAt<uint32_t>(cache.bytes, cache.bytes.size() - 4U, 99);
  EXPECT_FALSE(validates(cache.bytes));

  cache = cacheWithPage(emptyPage());
  SectionCacheValidation::Layout layout;
  ASSERT_TRUE(validates(cache.bytes, &layout));
  writeAt<uint32_t>(cache.bytes, cache.pageLut, cache.pageLut);
  MemoryInput changed(cache.bytes);
  uint64_t begin = 0;
  uint64_t end = 0;
  EXPECT_FALSE(SectionCacheValidation::pageBounds(changed, layout, 0, begin, end));
}
