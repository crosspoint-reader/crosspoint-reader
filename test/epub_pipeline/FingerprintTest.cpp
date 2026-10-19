// Fingerprint invalidation:
// the cache key is path-derived, so a book replaced in place must invalidate
// the cache, while an mtime-only touch must not. Observability: a canary file
// planted inside the cache dir survives iff the cache was NOT wiped.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/BookFonts.h"

namespace fs = std::filesystem;

namespace {

const char* kBookA = CORPUS_DIR "/test_headings.epub";
const char* kBookB = CORPUS_DIR "/test_font_sizes.epub";

struct FingerprintFixture : testing::Test {
  fs::path work;
  fs::path bookPath;
  std::string cacheDir;

  void SetUp() override {
    // Per-test dir: ctest -j runs these cases as parallel processes, so a
    // shared path would make them clobber each other's caches.
    work = fs::temp_directory_path() /
           (std::string("epub_fingerprint_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    bookPath = work / "book.epub";
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
  }
  void TearDown() override { fs::remove_all(work); }

  // Loads the book at bookPath, returns its parsed title ("" on failure).
  std::string loadTitle() {
    Epub epub(bookPath.string(), cacheDir);
    if (!epub.load(true)) return {};
    return epub.getTitle();
  }

  fs::path epubCacheRoot() {
    // Epub keys its cache dir by path hash; there is exactly one entry under cacheDir.
    for (const auto& e : fs::directory_iterator(cacheDir)) {
      if (e.is_directory()) return e.path();
    }
    return {};
  }

  void plantCanary() { std::ofstream(epubCacheRoot() / "canary") << "x"; }
  bool canaryAlive() { return fs::exists(epubCacheRoot() / "canary"); }
};

TEST_F(FingerprintFixture, ContentSwapAtSamePathRebuildsCache) {
  fs::copy_file(kBookA, bookPath);
  ASSERT_EQ(loadTitle(), "Heading Font Size Test");
  plantCanary();

  fs::remove(bookPath);
  fs::copy_file(kBookB, bookPath);
  // Same path, different bytes: the stale cache must be wiped and rebuilt —
  // the title MUST come from the new book, not the cached old one.
  EXPECT_EQ(loadTitle(), "Inline Font Size Test");
  EXPECT_FALSE(canaryAlive()) << "cache dir was not wiped on content change";
}

TEST_F(FingerprintFixture, MtimeTouchKeepsCache) {
  fs::copy_file(kBookA, bookPath);
  ASSERT_EQ(loadTitle(), "Heading Font Size Test");
  plantCanary();

  fs::last_write_time(bookPath, fs::file_time_type::clock::now());
  EXPECT_EQ(loadTitle(), "Heading Font Size Test");
  EXPECT_TRUE(canaryAlive()) << "mtime-only touch must not invalidate the cache";
}

TEST_F(FingerprintFixture, PreFingerprintCacheIsAdoptedNotWiped) {
  fs::copy_file(kBookA, bookPath);
  ASSERT_EQ(loadTitle(), "Heading Font Size Test");
  // Simulate a cache created by older firmware: no sidecar yet.
  fs::remove(epubCacheRoot() / "fingerprint.bin");
  plantCanary();

  EXPECT_EQ(loadTitle(), "Heading Font Size Test");
  EXPECT_TRUE(canaryAlive()) << "upgrade must adopt existing caches, not mass-invalidate";
  EXPECT_TRUE(fs::exists(epubCacheRoot() / "fingerprint.bin")) << "sidecar not written on adoption";
}

// The reader chooses a book's fonts from what BookFonts found in it (fonts.bin, in the cache) before load(). A book
// replaced under the same name must not be drawn in the old book's fonts: its stale cache goes first, and the new
// book is examined at this open.
TEST_F(FingerprintFixture, AReplacedBookForgetsTheOldBooksFontsBeforeLoad) {
  fs::copy_file(kBookA, bookPath);
  std::string cachePath;
  {
    Epub epub(bookPath.string(), cacheDir);
    ASSERT_TRUE(epub.load(true));
    ASSERT_EQ(BookFonts::State::None, BookFonts::prepare(epub, nullptr));
    cachePath = epub.getCachePath();
  }
  {
    const Epub same(bookPath.string(), cacheDir);
    EXPECT_FALSE(same.clearCacheIfContentChanged());
    EXPECT_EQ(BookFonts::State::None, BookFonts::read(cachePath)) << "the book as it was keeps what was found";
  }

  fs::remove(bookPath);
  fs::copy_file(kBookB, bookPath);
  ASSERT_EQ(BookFonts::State::None, BookFonts::read(cachePath)) << "until then the old book's answer stands";
  const Epub replaced(bookPath.string(), cacheDir);
  EXPECT_TRUE(replaced.clearCacheIfContentChanged());
  EXPECT_EQ(BookFonts::State::Pending, BookFonts::read(cachePath)) << "the old book's fonts.bin is gone";
  EXPECT_TRUE(replaced.needsFirstOpenIndexing());
}

TEST_F(FingerprintFixture, NeedsFirstOpenIndexingReportsContentSwap) {
  fs::copy_file(kBookA, bookPath);
  ASSERT_EQ(loadTitle(), "Heading Font Size Test");
  {
    Epub epub(bookPath.string(), cacheDir);
    EXPECT_FALSE(epub.needsFirstOpenIndexing());
  }
  fs::remove(bookPath);
  fs::copy_file(kBookB, bookPath);
  {
    Epub epub(bookPath.string(), cacheDir);
    EXPECT_TRUE(epub.needsFirstOpenIndexing()) << "caller must get its progress popup before the rebuild";
  }
}

}  // namespace
