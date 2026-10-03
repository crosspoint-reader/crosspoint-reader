#include <Epub/SectionAnchorMap.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <filesystem>

namespace {

class SectionAnchorMapTest : public ::testing::Test {
 protected:
  std::string path;
  void SetUp() override {
    path = (std::filesystem::temp_directory_path() /
            (::testing::UnitTest::GetInstance()->current_test_info()->name() + std::string(".bin")))
               .string();
  }
  void TearDown() override { std::filesystem::remove(path); }
  void writeMap(const std::vector<std::pair<std::string, uint16_t>>& anchors, const std::string& requested,
                const bool partial = false, const uint16_t pages = 10) {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
    ASSERT_TRUE(sectionAnchors::write(file, anchors, requested, pages, partial));
  }
  sectionAnchors::Lookup lookup(const std::string& anchor) {
    HalFile file;
    EXPECT_TRUE(Storage.openFileForRead("TEST", path, file));
    return sectionAnchors::read(file, anchor);
  }
};

TEST_F(SectionAnchorMapTest, LegacyMissStillRequiresBuild) {
  writeMap({{"ordinary", 3}}, "");
  const auto result = lookup("missing");
  EXPECT_FALSE(result.checked);
  EXPECT_FALSE(result.page.has_value());
}

TEST_F(SectionAnchorMapTest, CompletedMissingFragmentSurvivesReopenWithoutAPage) {
  writeMap({{"ordinary", 3}}, "missing");
  for (int visit = 0; visit < 3; ++visit) {
    const auto result = lookup("missing");
    EXPECT_TRUE(result.checked);
    EXPECT_FALSE(result.page.has_value());
  }
  const auto other = lookup("other");
  EXPECT_FALSE(other.checked);
  EXPECT_FALSE(other.page.has_value());
  const auto ordinary = lookup("ordinary");
  EXPECT_TRUE(ordinary.checked);
  EXPECT_EQ(ordinary.page, 3);
}

TEST_F(SectionAnchorMapTest, FoundRequestedFragmentHasNoNegativeRecord) {
  writeMap({{"ordinary", 3}, {"span", 7}}, "span");
  EXPECT_EQ(lookup("span").page, 7);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", path, file));
  uint16_t count = 0;
  serialization::readPod(file, count);
  EXPECT_EQ(count, 2);
}

TEST_F(SectionAnchorMapTest, PartialCannotCertifyAbsenceOrExposeUnwrittenPage) {
  writeMap({{"written", 2}, {"unwritten", 8}}, "unwritten", true, 5);
  EXPECT_EQ(lookup("written").page, 2);
  EXPECT_FALSE(lookup("unwritten").checked);
  EXPECT_FALSE(lookup("absent").checked);
}

TEST_F(SectionAnchorMapTest, MissingRecordKeepsExistingEncodingForOlderReaders) {
  writeMap({}, "missing");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", path, file));
  uint16_t count = 0;
  serialization::readPod(file, count);
  ASSERT_EQ(count, 1);
  std::string key;
  uint16_t page = 0;
  serialization::readString(file, key);
  serialization::readPod(file, page);
  EXPECT_NE(key, "missing");
  EXPECT_EQ(key, std::string("\x01") + "missing");
  EXPECT_EQ(page, 0);
  EXPECT_EQ(file.available(), 0);
}

TEST_F(SectionAnchorMapTest, LongKeysAreMatchedExactlyAcrossReadChunks) {
  const auto prefix = std::string(96, 'a');
  writeMap({{prefix + "found", 4}, {"", 0}}, prefix + "missing");
  EXPECT_EQ(lookup(prefix + "found").page, 4);
  EXPECT_TRUE(lookup(prefix + "missing").checked);
  EXPECT_FALSE(lookup(prefix + "missing").page.has_value());
  EXPECT_FALSE(lookup(prefix + "mismatch").checked);
  EXPECT_FALSE(lookup(prefix).checked);
  EXPECT_FALSE(lookup("").checked);
}

TEST_F(SectionAnchorMapTest, TruncatedRecordCannotCertifyAbsence) {
  {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
    serialization::writePod(file, static_cast<uint16_t>(1));
    serialization::writePod(file, static_cast<uint32_t>(1000));
    file.write(sectionAnchors::MISSING_PREFIX);
    file.write("missing", 7);
  }
  EXPECT_FALSE(lookup("missing").checked);
}

}  // namespace
