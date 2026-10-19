// ImageBlock::PlaceholderOnlyScope as the next-page pre-render uses it: images replay from their
// pixel caches or draw a placeholder, never a decode, and the scope counts the placeholders so the
// pre-render can refuse to show a page that has one.
//
// The case that matters is the third: the pre-render admits a page only when every image's cache
// EXISTS, but a cache can exist and still fail to replay (an out-of-date version is deleted on
// read). Only the draw finds that out, and it must neither decode nor go uncounted.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_png_images.epub";
const char* kEntry = "OEBPS/images/scaling_test.png";

struct PlaceholderOnlyScopeFixture : testing::Test {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("placeholder_scope_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  ImageBlock blockAt(const fs::path& target) const { return ImageBlock(target.string(), 120, 160, "", kBook, kEntry); }
};

TEST_F(PlaceholderOnlyScopeFixture, ACachedImageReplaysWithoutAPlaceholder) {
  ImageBlock block = blockAt(work / "img.png");
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);  // decode, writes the cache
  ASSERT_TRUE(block.hasPixelCache());

  ImageBlock::PlaceholderOnlyScope scope;
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_EQ(scope.placeholdersDrawn(), 0u);
}

TEST_F(PlaceholderOnlyScopeFixture, AnUncachedImageIsCountedAndNotDecoded) {
  const fs::path target = work / "img.png";
  ImageBlock block = blockAt(target);

  ImageBlock::PlaceholderOnlyScope scope;
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_EQ(scope.placeholdersDrawn(), 1u);
  EXPECT_FALSE(block.hasPixelCache()) << "a decode would have written the cache";
  EXPECT_FALSE(fs::exists(target)) << "nor may it extract the image to decode it later";
}

TEST_F(PlaceholderOnlyScopeFixture, ACacheThatExistsButFailsToReplayIsCountedAndNotDecoded) {
  const fs::path target = work / "img.png";
  {
    // The legacy header began with the width, high bit clear: renderFromCache() deletes such a
    // file and reports a miss.
    std::ofstream stale((work / "img.1bit.pxc").string(), std::ios::binary);
    const uint16_t header[3] = {120, 160, 0};
    stale.write(reinterpret_cast<const char*>(header), sizeof(header));
  }
  ImageBlock block = blockAt(target);
  ASSERT_TRUE(block.hasPixelCache()) << "the pre-render's up-front check would admit this page";

  ImageBlock::PlaceholderOnlyScope scope;
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_EQ(scope.placeholdersDrawn(), 1u);
  EXPECT_FALSE(block.hasPixelCache()) << "the stale cache is dropped and no decode replaces it";
  EXPECT_FALSE(fs::exists(target));
}

// --- Short of memory (the reader's memory ladder) ---------------------------------------------

struct LowMemoryFixture : PlaceholderOnlyScopeFixture {
  void SetUp() override {
    PlaceholderOnlyScopeFixture::SetUp();
    ImageBlock::takeLowMemoryPlaceholders();
  }
  void TearDown() override {
    ESP.setFreeHeap(200 * 1024);
    PlaceholderOnlyScopeFixture::TearDown();
  }
};

TEST_F(LowMemoryFixture, ADecodeTheHeapRefusesIsDrawnAsALowMemoryBoxAndCountedShortOfMemory) {
  ImageBlock block = blockAt(work / "img.png");
  ESP.setFreeHeap(8 * 1024);  // under the PNG decoder's floor
  const uint32_t before = image_scratch::heapShortCount();
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_FALSE(block.hasPixelCache());
  EXPECT_GT(image_scratch::heapShortCount(), before);
  EXPECT_EQ(1u, ImageBlock::takeLowMemoryPlaceholders()) << "not left blank";
  EXPECT_EQ(0u, ImageBlock::takeLowMemoryPlaceholders()) << "taken once";
  // With the memory back it decodes, and nothing says it is short.
  ESP.setFreeHeap(200 * 1024);
  const uint32_t after = image_scratch::heapShortCount();
  block.render(renderer, 0, 0, true, true);
  EXPECT_TRUE(block.hasPixelCache());
  EXPECT_EQ(after, image_scratch::heapShortCount());
  EXPECT_EQ(0u, ImageBlock::takeLowMemoryPlaceholders());
}

TEST_F(LowMemoryFixture, AnImageThatCannotBeDecodedAtAllIsNotShortOfMemory) {
  const fs::path target = work / "broken.png";
  {
    std::ofstream broken(target.string(), std::ios::binary);
    broken << "\x89PNG\r\n\x1a\n not a png at all";
  }
  ImageBlock block(target.string(), 120, 160, "");
  const uint32_t before = image_scratch::heapShortCount();
  block.render(renderer, 0, 0, true, true);
  EXPECT_EQ(before, image_scratch::heapShortCount()) << "more memory would not decode it: the ladder leaves it";
  EXPECT_EQ(0u, ImageBlock::takeLowMemoryPlaceholders());
}

TEST_F(LowMemoryFixture, ALowMemoryScopeDrawsTheUncachedImagesAsBoxesWithoutADecode) {
  const fs::path target = work / "img.png";
  ImageBlock block = blockAt(target);
  ImageBlock::PlaceholderOnlyScope scope(ImageBlock::Placeholder::LowMemory);
  block.render(renderer, 0, 0, true, true);
  EXPECT_EQ(1u, scope.placeholdersDrawn());
  EXPECT_EQ(1u, ImageBlock::takeLowMemoryPlaceholders());
  EXPECT_FALSE(block.hasPixelCache());
  EXPECT_FALSE(fs::exists(target));
}

TEST_F(PlaceholderOnlyScopeFixture, EachScopeStartsCountingFromZero) {
  ImageBlock block = blockAt(work / "img.png");
  {
    ImageBlock::PlaceholderOnlyScope first;
    block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
    ASSERT_EQ(first.placeholdersDrawn(), 1u);
  }
  ImageBlock::PlaceholderOnlyScope second;
  EXPECT_EQ(second.placeholdersDrawn(), 0u);
}

}  // namespace
