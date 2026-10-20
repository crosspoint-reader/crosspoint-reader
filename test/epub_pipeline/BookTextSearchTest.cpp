// In-book full-text search (lib/Epub/Epub/BookTextSearch). Two halves:
//
//  - Unit tests of the matcher on small XHTML strings: folding, markup, entities, caps,
//    snippets and streaming.
//  - Corpus cross-checks: the searcher must count every chapter's visible bytes exactly as
//    the KOReader XPath indexer does (VisibleText.h's rule, shared with the layout parser),
//    and a word taken from a laid-out page must be found on that page through
//    Section::getPageForVisibleTextOffset -- the lookup the reader uses to jump to a hit.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/BookTextSearch.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "GfxRenderer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"

namespace fs = std::filesystem;
using booksearch::ChapterSearcher;
using booksearch::foldQuery;
using booksearch::Hit;

namespace {

std::vector<Hit> search(const std::string& xhtml, const std::string& query, size_t maxHits = 50,
                        bool byteAtATime = false) {
  ChapterSearcher searcher(foldQuery(query), maxHits);
  EXPECT_TRUE(searcher.ok());
  if (byteAtATime) {
    for (const char c : xhtml) searcher.write(static_cast<uint8_t>(c));
  } else {
    searcher.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
  }
  searcher.finish();
  return searcher.hits();
}

std::string doc(const std::string& body) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>T"
         "</title></head><body>" +
         body + "</body></html>";
}

std::vector<uint32_t> cps(const char32_t* s) {
  std::vector<uint32_t> out;
  for (; *s; ++s) out.push_back(static_cast<uint32_t>(*s));
  return out;
}

}  // namespace

TEST(BookTextSearchTest, QueryFoldsCaseAccentsAndWhitespace) {
  EXPECT_EQ(foldQuery("  Café   CRÈME\tbrûlée "), cps(U"cafe creme brulee"));
  EXPECT_EQ(foldQuery("Łódź Žižkov"), cps(U"lodz zizkov"));
  EXPECT_EQ(foldQuery("it’s “fine” — ok"), cps(U"it's \"fine\" - ok"));
  EXPECT_EQ(foldQuery("ПРИВЕТ Ωμέγα"), cps(U"привет ωμέγα"));
  EXPECT_EQ(foldQuery("co­op"), cps(U"coop"));  // soft hyphen ignored
  EXPECT_TRUE(foldQuery("   ­ ").empty());
}

TEST(BookTextSearchTest, OffsetsCountNonWhitespaceBytes) {
  // "Hello world": 5 bytes before the space, which counts nothing.
  const auto hits = search(doc("<p>Hello world</p>"), "WORLD");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 5u);
  EXPECT_EQ(hits[0].visibleEnd, 10u);
  // Multi-byte characters count every byte: "Ünï" is 5 bytes (2 + 1 + 2).
  const auto wide = search(doc("<p>Ünï code</p>"), "code");
  ASSERT_EQ(wide.size(), 1u);
  EXPECT_EQ(wide[0].visibleOffset, 5u);
}

TEST(BookTextSearchTest, HeadAndMarkupWhitespaceAreNotCounted) {
  const auto hits = search("<html><head><title>abc</title></head><body>\n  <p>Hi there</p>\n</body></html>", "there");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 2u);  // only "Hi" before it
}

TEST(BookTextSearchTest, MatchesAcrossInlineMarkup) {
  const auto hits = search(doc("<p>Hel<i>lo</i> wor<b>ld</b></p>"), "hello world");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 0u);
  EXPECT_EQ(hits[0].visibleEnd, 10u);
}

TEST(BookTextSearchTest, BlockBoundariesSeparateWords) {
  EXPECT_TRUE(search(doc("<p>end</p><p>start</p>"), "endstart").empty());
  const auto hits = search(doc("<p>end</p><p>start</p>"), "end start");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 0u);
  EXPECT_EQ(hits[0].visibleEnd, 8u);
  EXPECT_EQ(search(doc("<p>line one<br>line two</p>"), "one line").size(), 1u);  // bare <br> repaired
}

TEST(BookTextSearchTest, IgnoresScriptAndStyleText) {
  const auto hits = search(doc("<style>.needle{}</style><script>var needle;</script><p>needle</p>"), "needle");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 0u);
}

TEST(BookTextSearchTest, ExpandsHtmlEntitiesLikeTheLayoutParser) {
  const auto hits = search(doc("<p>caf&eacute;&nbsp;&amp; more</p>"), "cafe & more");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].visibleOffset, 0u);
  // café (5 bytes) + nbsp (2 bytes, not ASCII space) + & (1) + more (4)
  EXPECT_EQ(hits[0].visibleEnd, 12u);
}

TEST(BookTextSearchTest, AccentsCaseAndQuotesInTheBookMatchAPlainQuery) {
  EXPECT_EQ(search(doc("<p>The CAFÉ was open.</p>"), "cafe").size(), 1u);
  EXPECT_EQ(search(doc("<p>He said “hello”.</p>"), "\"hello\"").size(), 1u);
  EXPECT_EQ(search(doc("<p>two\n\n   words</p>"), "two words").size(), 1u);
}

TEST(BookTextSearchTest, NonOverlappingAndCapped) {
  EXPECT_EQ(search(doc("<p>aaaa</p>"), "aa").size(), 2u);
  EXPECT_EQ(search(doc("<p>cat cat cat cat cat cat</p>"), "cat", 3).size(), 3u);
}

TEST(BookTextSearchTest, SnippetShowsContextAroundTheMatch) {
  const auto hits = search(doc("<p>It was the best of times, it was the worst of times.</p>"), "best");
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_NE(hits[0].snippet.find("the best of"), std::string::npos);
  EXPECT_EQ(hits[0].snippet.rfind("It was", 0), 0u);  // whole lead-in fits: no leading ellipsis
}

TEST(BookTextSearchTest, ByteAtATimeMatchesWholeBuffer) {
  const std::string text = doc("<p>Ünïcödé <em>spans</em> and caf&eacute;</p>");
  const auto whole = search(text, "unicode spans");
  const auto bytes = search(text, "unicode spans", 50, true);
  ASSERT_EQ(whole.size(), 1u);
  ASSERT_EQ(bytes.size(), 1u);
  EXPECT_EQ(whole[0].visibleOffset, bytes[0].visibleOffset);
  EXPECT_EQ(whole[0].visibleEnd, bytes[0].visibleEnd);
}

TEST(BookTextSearchTest, AbortProbeStopsAndReports) {
  ChapterSearcher searcher(foldQuery("x"), 10);
  searcher.setAbortProbe([] { return true; });
  const std::string text = doc("<p>x x x</p>");
  searcher.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  searcher.finish();
  EXPECT_TRUE(searcher.aborted());
  EXPECT_TRUE(*searcher.stopFlag());
  EXPECT_TRUE(searcher.hits().empty());
}

// --- Corpus cross-checks -----------------------------------------------------------------

namespace {

std::vector<std::string> corpusBooks() {
  std::vector<std::string> books;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") books.push_back(entry.path().string());
  }
  std::sort(books.begin(), books.end());
  return books;
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  return p;
}

// A word worth searching for: at least five ASCII letters, nothing else (no hyphenation
// fragment, punctuation or digits).
bool searchableWord(const char* w) {
  size_t n = 0;
  for (; w[n]; ++n) {
    if (!std::isalpha(static_cast<unsigned char>(w[n]))) return false;
  }
  return n >= 5;
}

struct SearchCorpus : testing::TestWithParam<std::string> {
  fs::path work;
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void SetUp() override {
    std::string tag = testing::UnitTest::GetInstance()->current_test_info()->name();
    for (char& c : tag) {
      if (c == '/') c = '_';
    }
    work = fs::temp_directory_path() / "book_text_search" / tag;
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(GetParam(), (work / "cache").string());
    ASSERT_TRUE(epub->load(true)) << GetParam();
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }

  std::vector<Hit> searchSpine(const int spine, const std::string& query, uint32_t* counted = nullptr) {
    ChapterSearcher searcher(foldQuery(query), 1000);
    EXPECT_TRUE(searcher.ok());
    const auto href = epub->getSpineItem(spine).href;
    EXPECT_TRUE(epub->readItemContentsToStream(href, searcher, 1024, searcher.stopFlag())) << "spine " << spine;
    searcher.finish();
    if (counted) *counted = searcher.countedBytes();
    return searcher.takeHits();
  }
};

}  // namespace

TEST_P(SearchCorpus, CountsVisibleBytesLikeTheXPathIndexer) {
  for (int spine = 0; spine < epub->getSpineItemsCount(); ++spine) {
    const auto expected = ChapterXPathIndexerInternal::countTotalTextBytes(epub, spine);
    if (!expected.has_value()) continue;  // a chapter the indexer cannot read either
    uint32_t counted = 0;
    // A query that occurs nowhere, so the whole chapter is streamed.
    searchSpine(spine, "\xEE\x80\x80zzqx", &counted);
    EXPECT_EQ(counted, *expected) << "spine " << spine;
  }
}

TEST_P(SearchCorpus, WordsFromAPageAreFoundOnThatPage) {
  int checked = 0;
  for (int spine = 0; spine < epub->getSpineItemsCount() && checked < 40; ++spine) {
    auto section = std::make_unique<Section>(epub, spine, renderer);
    if (!section->createSectionFile(params(), {}, /*skipEviction=*/true) || !section->loadSectionFile(params())) {
      continue;
    }
    for (int pageIndex = 0; pageIndex < section->pageCount && pageIndex < 6; ++pageIndex) {
      section->currentPage = pageIndex;
      const auto page = section->loadPageFromSectionFile();
      if (!page) continue;
      std::string word;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const TextBlock* block = static_cast<const PageLine&>(*element).getBlock();
        for (uint16_t i = 0; block && i < block->wordCount() && word.empty(); ++i) {
          if (searchableWord(block->wordText(i))) word = block->wordText(i);
        }
        if (!word.empty()) break;
      }
      if (word.empty()) continue;
      const auto hits = searchSpine(spine, word);
      const bool landsHere = std::any_of(hits.begin(), hits.end(), [&](const Hit& h) {
        return section->getPageForVisibleTextOffset(h.visibleOffset) == std::optional<uint16_t>(pageIndex);
      });
      EXPECT_TRUE(landsHere) << "\"" << word << "\" from spine " << spine << " page " << pageIndex << " ("
                             << hits.size() << " hits)";
      ++checked;
    }
  }
  RecordProperty("words_checked", checked);
  std::printf("[ WORDS    ] %s: %d\n", fs::path(GetParam()).stem().c_str(), checked);
}

INSTANTIATE_TEST_SUITE_P(Corpus, SearchCorpus, testing::ValuesIn(corpusBooks()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           std::string name = fs::path(info.param).stem().string();
                           for (char& c : name) {
                             if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                           }
                           return name;
                         });
