// Cover for the TOC boundaries a section holds right after it was BUILT, not reloaded.
//
// The reader keeps using the Section it just built (EpubReaderActivity's createSectionFile path
// never reloads it), so the status bar's chapter title and the chapter menu read these
// boundaries straight away. They were resolved from the parser's resident anchors, which have
// been empty since anchors spill to SD: every TOC entry of a spine with sub-chapters started at
// page 0, so the whole spine was named after its last sub-chapter until the book was reopened.
// A section loaded from the cache resolved them from the file and was right, which is what this
// compares against.
#include <Arduino.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_spine_toc_edges.epub";
// chapter3.xhtml: one TOC entry for the file plus #the-reef, #the-current and #the-depths.
constexpr int kChapter3Spine = 6;

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

std::string freshCacheDir() {
  // Per process: other worktrees may run this suite at the same time.
  const auto dir = fs::temp_directory_path() / ("toc_boundaries_after_build_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

}  // namespace

TEST(TocBoundariesAfterBuild, ABuiltSectionNamesTheSameChapterPerPageAsALoadedOne) {
  auto epub = std::make_shared<Epub>(kFixture, freshCacheDir());
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();

  const int firstToc = epub->getTocIndexForSpineIndex(kChapter3Spine);
  ASSERT_GE(firstToc, 0);
  int lastToc = firstToc;
  while (lastToc + 1 < epub->getTocItemsCount() && epub->getTocItem(lastToc + 1).spineIndex == kChapter3Spine) {
    ++lastToc;
  }
  ASSERT_EQ(lastToc - firstToc, 3) << "fixture: chapter 3 has three anchored sub-chapters";

  GfxRenderer renderer;
  const auto p = params();
  Section built(epub, kChapter3Spine, renderer);
  ASSERT_TRUE(built.createSectionFile(p, {}, /*skipEviction=*/true));

  Section loaded(epub, kChapter3Spine, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(p));
  ASSERT_EQ(built.pageCount, loaded.pageCount);

  // The loaded section is the reference; it must have resolved the sub-chapters past page 0.
  const auto lastRange = loaded.getPageRangeForTocIndex(lastToc);
  ASSERT_TRUE(lastRange.has_value());
  ASSERT_GT(lastRange->startPage, 0) << "fixture: the last sub-chapter must start after page 0";

  for (int toc = firstToc; toc <= lastToc; ++toc) {
    const auto want = loaded.getPageRangeForTocIndex(toc);
    const auto got = built.getPageRangeForTocIndex(toc);
    ASSERT_TRUE(want.has_value()) << "toc " << toc;
    ASSERT_TRUE(got.has_value()) << "toc " << toc;
    EXPECT_EQ(got->startPage, want->startPage) << "toc " << toc;
    EXPECT_EQ(got->endPage, want->endPage) << "toc " << toc;
  }
  for (int page = 0; page < loaded.pageCount; ++page) {
    EXPECT_EQ(built.getTocIndexForPage(page), loaded.getTocIndexForPage(page)) << "page " << page;
  }
}
