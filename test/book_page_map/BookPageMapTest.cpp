#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "BookPageMap.h"

FakeHalStorage Storage;

namespace {

PageMapFingerprint fingerprint() {
  PageMapFingerprint fp;
  fp.sectionCacheVersion = 29;
  fp.fontId = 4;
  fp.lineCompression = 0.95f;
  fp.extraParagraphSpacing = true;
  fp.paragraphAlignment = 2;
  fp.viewportWidth = 450;
  fp.viewportHeight = 720;
  fp.hyphenationEnabled = true;
  fp.embeddedStyle = true;
  fp.imageRendering = 1;
  fp.focusReadingEnabled = false;
  return fp;
}

BookPageMap mapWithSizes(std::initializer_list<uint32_t> sizes) {
  BookPageMap map;
  EXPECT_TRUE(map.init(static_cast<int>(sizes.size()), fingerprint()));
  int index = 0;
  for (const uint32_t size : sizes) map.setSectionBytes(index++, size);
  return map;
}

class BookPageMapTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(BookPageMapTest, CalibratesUnknownSectionsFromExactCounts) {
  auto map = mapWithSizes({4000, 6000});

  EXPECT_EQ(map.total(), 5);
  EXPECT_TRUE(map.recordSection(0, 4));
  EXPECT_FALSE(map.recordSection(0, 4));
  EXPECT_EQ(map.total(), 10);
  EXPECT_FALSE(map.isExact());

  EXPECT_TRUE(map.recordSection(1, 6));
  EXPECT_EQ(map.total(), 10);
  EXPECT_TRUE(map.isExact());
}

TEST_F(BookPageMapTest, ExactEmptySectionDoesNotSkewCalibrationOrPrefix) {
  auto map = mapWithSizes({1234, 4000});

  ASSERT_TRUE(map.recordSection(0, 0));
  EXPECT_EQ(map.total(), 2);
  EXPECT_EQ(map.globalPage(1, 0), 1);
}

TEST_F(BookPageMapTest, LiveLazyEstimateOverridesSeedWithoutBecomingExact) {
  auto map = mapWithSizes({20000});

  EXPECT_EQ(map.total(), 10);
  EXPECT_EQ(map.total(0, 37), 37);
  EXPECT_EQ(map.globalPage(0, 19, 37), 20);
  EXPECT_FALSE(map.isExact());

  ASSERT_TRUE(map.recordSection(0, 35));
  EXPECT_EQ(map.total(0, 37), 35);
  EXPECT_TRUE(map.isExact());
}

TEST_F(BookPageMapTest, AggregatesMixedExactAndEstimatedSections) {
  auto map = mapWithSizes({4000, 10000, 6000, 4000});
  ASSERT_TRUE(map.recordSection(0, 2));
  ASSERT_TRUE(map.recordSection(2, 3));

  EXPECT_EQ(map.total(), 12);
  EXPECT_EQ(map.globalPage(3, 1), 12);
  EXPECT_EQ(map.total(1, 7), 14);
  EXPECT_FALSE(map.isExact());
}

TEST_F(BookPageMapTest, PersistsExactCountsAtomically) {
  auto saved = mapWithSizes({4000, 6000, 1000});
  ASSERT_TRUE(saved.recordSection(0, 2));
  ASSERT_TRUE(saved.recordSection(2, 0));
  ASSERT_TRUE(saved.save("/pagemap.bin"));
  EXPECT_TRUE(Storage.exists("/pagemap.bin"));
  EXPECT_FALSE(Storage.exists("/pagemap.bin.tmp"));

  auto loaded = mapWithSizes({4000, 6000, 1000});
  ASSERT_TRUE(loaded.load("/pagemap.bin"));
  EXPECT_EQ(loaded.total(), 5);
  EXPECT_EQ(loaded.globalPage(1, 0), 3);
  EXPECT_FALSE(loaded.isExact());

  Storage.maxWritePerCall = 2;
  EXPECT_FALSE(saved.save("/pagemap.bin"));
  EXPECT_TRUE(Storage.exists("/pagemap.bin"));
  EXPECT_FALSE(Storage.exists("/pagemap.bin.tmp"));
}

TEST_F(BookPageMapTest, RejectsTruncatedAndMismatchedCachesAndClearsStaleCounts) {
  auto saved = mapWithSizes({4000, 6000});
  ASSERT_TRUE(saved.recordSection(0, 4));
  ASSERT_TRUE(saved.save("/pagemap.bin"));

  auto& bytes = Storage.files.at("/pagemap.bin");
  bytes.pop_back();

  auto loaded = mapWithSizes({4000, 6000});
  ASSERT_TRUE(loaded.recordSection(0, 99));
  EXPECT_FALSE(loaded.load("/pagemap.bin"));
  EXPECT_EQ(loaded.total(), 5);

  ASSERT_TRUE(saved.save("/valid.bin"));
  auto otherFingerprint = fingerprint();
  otherFingerprint.viewportWidth++;
  BookPageMap mismatched;
  ASSERT_TRUE(mismatched.init(2, otherFingerprint));
  mismatched.setSectionBytes(0, 4000);
  mismatched.setSectionBytes(1, 6000);
  ASSERT_TRUE(mismatched.recordSection(0, 99));
  EXPECT_FALSE(mismatched.load("/valid.bin"));
  EXPECT_EQ(mismatched.total(), 5);
}

TEST_F(BookPageMapTest, SaturatesLargeEstimatesInsteadOfOverflowing) {
  BookPageMap many;
  ASSERT_TRUE(many.init(1001, fingerprint()));
  for (int i = 0; i < many.sectionCount(); ++i) {
    many.setSectionBytes(i, std::numeric_limits<uint32_t>::max());
  }
  EXPECT_EQ(many.total(), std::numeric_limits<int>::max());
  EXPECT_EQ(many.globalPage(1000, 1), std::numeric_limits<int>::max());

  auto extremeRatio = mapWithSizes({1, std::numeric_limits<uint32_t>::max()});
  ASSERT_TRUE(extremeRatio.recordSection(0, std::numeric_limits<uint16_t>::max()));
  EXPECT_EQ(extremeRatio.total(), std::numeric_limits<int>::max());
}

TEST_F(BookPageMapTest, RejectsUnsupportedSectionCounts) {
  BookPageMap map;
  EXPECT_FALSE(map.init(-1, fingerprint()));
  EXPECT_FALSE(map.init(static_cast<int>(std::numeric_limits<uint16_t>::max()) + 1, fingerprint()));
  EXPECT_EQ(map.sectionCount(), 0);
}

}  // namespace
