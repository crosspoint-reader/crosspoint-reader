// A chapter long enough to grow both per-page build tables -- Section's page LUT and the parser's
// paragraph LUT -- far past the build's page estimate and past the old 512-page reserve, so they
// run through many chunks (lib/Memory/ChunkedVector.h).
//
// Before the tables were chunked, every growth past the reserve was a std::vector doubling: an
// 8 KB plus a 4 KB block past 512 pages, asked for through the throwing operator new while a
// borrowed build had the heap at a few KB contiguous. On the device that aborts, and the chapter
// rebuilt from page 0 on the next open and aborted again (crosspoint-reader PR #3027: a 1300-page
// chapter). The host has the heap to grow either way, so the second test refuses chunk
// allocations on purpose (CHUNKED_VECTOR_FAULT_INJECTION) and checks what a refusal leaves: a
// truncated chapter whose pages, page LUT and paragraph LUT all stop at the same page.
#include <ChunkedVector.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"

namespace fs = std::filesystem;

namespace {

constexpr int kParagraphs = 2400;

// Two of the stub font's 24 px lines a page, so each one-word paragraph is about half a page.
Section::BuildParams shortPages() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 48;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::string word(const int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "w%04d", i);
  return buf;
}

std::string longChapterBody() {
  std::string body;
  body.reserve(kParagraphs * 16);
  for (int i = 0; i < kParagraphs; ++i) body += "<p>" + word(i) + "</p>\n";
  return body;
}

// Per process, so test runs in parallel worktrees never share a book or a cache.
fs::path freshWorkDir(const std::string& tag) {
  const auto dir =
      fs::temp_directory_path() / ("long_chapter_page_tables_" + std::to_string(static_cast<long>(getpid()))) / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// Every word of every page, in page order. Loading each page goes through the page LUT.
std::vector<std::string> wordsOfAllPages(Section& section) {
  std::vector<std::string> words;
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    EXPECT_TRUE(page) << "page " << pg;
    if (!page) break;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& block = static_cast<const PageLine&>(*el).getBlock();
      if (!block || !block->valid()) continue;
      for (uint16_t w = 0; w < block->wordCount(); ++w) words.emplace_back(block->wordText(w));
    }
  }
  return words;
}

// The chapter's words up to `count`, in order: what the pages must hold, with nothing lost or
// repeated where one chunk of either table ends and the next begins.
void expectChapterPrefix(const std::vector<std::string>& words, const size_t count) {
  ASSERT_EQ(words.size(), count);
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(words[i], word(static_cast<int>(i))) << "word " << i;
  }
}

// The paragraph LUT as the cache stores it: an entry for every page, starts strictly increasing
// (each page here opens on a new word), and each start looks up its own page.
void expectParagraphLutCoversEveryPage(const Section& section) {
  uint32_t previousStart = 0;
  uint16_t previousParagraph = 0;
  for (uint16_t pg = 0; pg < section.pageCount; ++pg) {
    const auto start = section.getVisibleTextOffsetForPage(pg);
    const auto paragraph = section.getParagraphIndexForPage(pg);
    ASSERT_TRUE(start.has_value()) << "page " << pg;
    ASSERT_TRUE(paragraph.has_value()) << "page " << pg;
    if (pg > 0) {
      EXPECT_GT(*start, previousStart) << "page " << pg;
      EXPECT_GE(*paragraph, previousParagraph) << "page " << pg;
    }
    EXPECT_EQ(section.getPageForVisibleTextOffset(*start), std::optional<uint16_t>(pg)) << "page " << pg;
    previousStart = *start;
    previousParagraph = *paragraph;
  }
  EXPECT_FALSE(section.getVisibleTextOffsetForPage(section.pageCount).has_value());
}

// Restores "never refuse" however the test ends.
struct AllocationsLeft {
  explicit AllocationsLeft(const size_t n) { chunkedVectorAllocationsLeft() = n; }
  ~AllocationsLeft() { chunkedVectorAllocationsLeft() = SIZE_MAX; }
  AllocationsLeft(const AllocationsLeft&) = delete;
  AllocationsLeft& operator=(const AllocationsLeft&) = delete;
};

}  // namespace

TEST(LongChapterPageTables, AChapterFarPastTheReserveBuildsWhole) {
  const auto work = freshWorkDir("whole");
  auto epub = syntheticBook(work, 0, longChapterBody());
  ASSERT_TRUE(epub);
  GfxRenderer renderer;
  const auto p = shortPages();

  Section built(epub, 0, renderer);
  ASSERT_TRUE(built.createSectionFile(p, {}, /*skipEviction=*/true));
  EXPECT_FALSE(built.isTruncatedCache());
  // Well past the 512-page reserve cap and twenty times the estimate for these few bytes: both
  // tables ran through their reserved chunk and a run of 1 KB ones.
  ASSERT_GT(built.pageCount, 1024);
  // The page LUT the build handed over to the section: every page loads through it.
  expectChapterPrefix(wordsOfAllPages(built), kParagraphs);

  // And the same chapter read back cold from its cache, LUT and paragraph LUT from the file.
  Section loaded(epub, 0, renderer);
  ASSERT_TRUE(loaded.loadSectionFile(p));
  ASSERT_EQ(loaded.pageCount, built.pageCount);
  EXPECT_FALSE(loaded.isTruncatedCache());
  expectChapterPrefix(wordsOfAllPages(loaded), kParagraphs);
  expectParagraphLutCoversEveryPage(loaded);
  epub.reset();
  fs::remove_all(work);
}

// Refuse the build's table chunks after `allowed` of them, for every early `allowed`: the refusal
// lands alternately on the paragraph LUT (its push comes first in emitPage) and on the page LUT
// (in Section's page callback, after the paragraph entry was taken and has to be given back).
// Either way the build stops at that page instead of aborting, and keeps everything before it.
TEST(LongChapterPageTables, ARefusedChunkStopsTheBuildWithBothTablesInStep) {
  const auto p = shortPages();
  GfxRenderer renderer;
  int previousPages = 0;
  for (size_t allowed = 0; allowed <= 12; ++allowed) {
    SCOPED_TRACE("chunk allocations allowed: " + std::to_string(allowed));
    // A book (and so a cache) of its own for every round: the cache is keyed on the path.
    const auto work = freshWorkDir("refused_" + std::to_string(allowed));
    auto epub = syntheticBook(work, static_cast<int>(allowed), longChapterBody());
    ASSERT_TRUE(epub);

    Section built(epub, 0, renderer);
    bool ok = false;
    {
      const AllocationsLeft guard(allowed);
      ok = built.createSectionFile(p, {}, /*skipEviction=*/true);
    }
    if (!ok) {
      // Refused before the first page was indexed: nothing to keep, the build fails as a build
      // with no pages does -- and is not cached.
      EXPECT_EQ(built.pageCount, 0);
      Section probe(epub, 0, renderer);
      EXPECT_FALSE(probe.loadSectionFile(p));
      continue;
    }
    ASSERT_GT(built.pageCount, 0);
    EXPECT_TRUE(built.isTruncatedCache()) << "the chapter stopped early and must say so";
    EXPECT_LT(built.pageCount, kParagraphs / 2);
    EXPECT_GE(built.pageCount, previousPages) << "more room never keeps fewer pages";
    previousPages = built.pageCount;

    const auto words = wordsOfAllPages(built);
    ASSERT_FALSE(words.empty());
    expectChapterPrefix(words, words.size());

    Section loaded(epub, 0, renderer);
    ASSERT_TRUE(loaded.loadSectionFile(p)) << "a truncated chapter is still a readable cache";
    ASSERT_EQ(loaded.pageCount, built.pageCount);
    EXPECT_TRUE(loaded.isTruncatedCache());
    EXPECT_EQ(wordsOfAllPages(loaded), words);
    expectParagraphLutCoversEveryPage(loaded);
    epub.reset();
    fs::remove_all(work);
  }
  EXPECT_GT(previousPages, 0) << "no round got far enough to keep a page";
}

// The chapter-scaled tables whose entries the chapter can do without -- printed-page labels and,
// when the anchor spill will not open, the resident anchors -- stay std containers, grown only
// when the heap has the block (heapReserveFor). On a heap without it the entries past that point
// are dropped and the section is marked simplified; the chapter itself builds whole. Unguarded,
// the growth went through the throwing operator new, an abort on the device.

TEST(LongChapterPageTables, PrintedPageLabelsPastTheHeapAreDroppedNotTheChapter) {
  // One printed-page marker per paragraph: the label pool reaches ~12 KB.
  std::string body;
  for (int i = 0; i < kParagraphs; ++i) {
    body += "<p><span epub:type=\"pagebreak\" title=\"" + std::to_string(i + 1) + "\"/>" + word(i) + "</p>\n";
  }
  const auto p = shortPages();
  GfxRenderer renderer;
  const auto lastLabel = [&](const int index, const size_t largestBlock, bool& simplified) {
    const auto work = freshWorkDir("labels_" + std::to_string(index));
    auto epub = syntheticBook(work, index, body);
    int label = -1;
    {
      Section built(epub, 0, renderer);
      bool ok = false;
      {
        const HostLargestFreeBlock heap(largestBlock);
        ok = built.createSectionFile(p, {}, /*skipEviction=*/true);
      }
      EXPECT_TRUE(ok);
      EXPECT_FALSE(built.isTruncatedCache()) << "a label is never worth the chapter";
      EXPECT_EQ(built.pageCount, kParagraphs / 2);
      Section loaded(epub, 0, renderer);
      EXPECT_TRUE(loaded.loadSectionFile(p));
      simplified = loaded.isSimplified();
      expectChapterPrefix(wordsOfAllPages(loaded), kParagraphs);
      if (const auto last = loaded.getNearestPrintedPageLabelAtOrBefore(loaded.pageCount - 1)) label = std::stoi(*last);
    }
    epub.reset();
    fs::remove_all(work);
    return label;
  };

  bool simplified = true;
  EXPECT_EQ(lastLabel(0, 200 * 1024, simplified), kParagraphs) << "with the heap, every label is kept";
  EXPECT_FALSE(simplified);
  // An 8 KB largest block: the pool doubles to 7,680 bytes and no further.
  const int kept = lastLabel(1, 8 * 1024, simplified);
  EXPECT_GT(kept, 0);
  EXPECT_LT(kept, kParagraphs);
  EXPECT_TRUE(simplified) << "the dropped labels are latched in the status byte";
}

TEST(LongChapterPageTables, ResidentAnchorsPastTheHeapAreDroppedNotTheChapter) {
  constexpr int kAnchors = 100;
  std::string body;
  for (int i = 0; i < kAnchors; ++i) body += "<p id=\"a" + std::to_string(i) + "\">" + word(i) + "</p>\n";
  const auto p = shortPages();
  GfxRenderer renderer;
  const auto build = [&](const int index, const size_t largestBlock, const auto& check) {
    const auto work = freshWorkDir("anchors_" + std::to_string(index));
    auto epub = syntheticBook(work, index, body);
    // A directory where the spill file goes: it will not open, so the anchors stay in RAM.
    fs::create_directories(Epub::spineCacheDir(epub->getCachePath(), 0) + "/anchors_0.tmp");
    {
      Section built(epub, 0, renderer);
      {
        const HostLargestFreeBlock heap(largestBlock);
        EXPECT_TRUE(built.createSectionFile(p, {}, /*skipEviction=*/true));
      }
      EXPECT_FALSE(built.isTruncatedCache());
      EXPECT_EQ(built.pageCount, kAnchors / 2);
      Section loaded(epub, 0, renderer);
      EXPECT_TRUE(loaded.loadSectionFile(p));
      check(loaded);
    }
    epub.reset();
    fs::remove_all(work);
  };

  build(0, 200 * 1024, [](const Section& s) {
    EXPECT_FALSE(s.isSimplified());
    EXPECT_EQ(s.getPageForAnchor("a" + std::to_string(kAnchors - 1)), std::optional<uint16_t>(kAnchors / 2 - 1));
  });
  // A 2 KB largest block: room for the 32-entry reserve, not for its doubling.
  build(1, 2 * 1024, [](const Section& s) {
    EXPECT_TRUE(s.isSimplified()) << "the dropped anchors are latched in the status byte";
    EXPECT_EQ(s.getPageForAnchor("a0"), std::optional<uint16_t>(0));
    EXPECT_EQ(s.getPageForAnchor("a31"), std::optional<uint16_t>(15));
    EXPECT_FALSE(s.getPageForAnchor("a" + std::to_string(kAnchors - 1)).has_value());
  });
}
