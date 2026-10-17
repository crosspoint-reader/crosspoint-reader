#include <Epub/Epub/converters/PixelCache.h>
#include <gtest/gtest.h>

#include <filesystem>

namespace {
class PixelCacheTest : public ::testing::Test {
 protected:
  std::string path;
  void SetUp() override {
    path = (std::filesystem::temp_directory_path() /
            (std::string("crosspoint-pixel-cache-") + ::testing::UnitTest::GetInstance()->current_test_info()->name()))
               .string();
    HalFileTest::failNextWrite = false;
    HalFileTest::failNextClose = false;
  }
  void TearDown() override {
    HalFileTest::failNextWrite = false;
    HalFileTest::failNextClose = false;
    Storage.remove(path);
  }
};
TEST_F(PixelCacheTest, PublishesCompleteCache) {
  PixelCache cache;
  ASSERT_TRUE(cache.begin(path, 4, 1, 0, 0, 1));
  EXPECT_TRUE(cache.finalize());
  EXPECT_TRUE(Storage.exists(path.c_str()));
  EXPECT_EQ(std::filesystem::file_size(path), 5u);
}
TEST_F(PixelCacheTest, RejectsFinalWriteFailure) {
  PixelCache cache;
  ASSERT_TRUE(cache.begin(path, 4, 1, 0, 0, 1));
  HalFileTest::failNextWrite = true;
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists(path.c_str()));
}
TEST_F(PixelCacheTest, RejectsCloseFailure) {
  PixelCache cache;
  ASSERT_TRUE(cache.begin(path, 4, 1, 0, 0, 1));
  HalFileTest::failNextClose = true;
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists(path.c_str()));
}
}  // namespace
