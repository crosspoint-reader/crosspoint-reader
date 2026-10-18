// The KOReader XPath mappers read a chapter straight from the inflate into the SAX parser: no
// temp file on the SD card, and when the parser has what it needs the inflate stops.
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "KOReaderSync/ChapterXPathIndexer.h"
#include "KOReaderSync/ChapterXPathIndexerInternal.h"
#include "SaxParser/SaxFeedSink.h"
#include "SaxParser/SaxParser.h"

namespace fs = std::filesystem;

namespace {

const std::string kBook = std::string(CORPUS_DIR) + "/test_spine_toc_edges.epub";
constexpr int kAppendixSpine = 12;  // 14 KB of XHTML

struct CountingState {
  size_t elements = 0;
  size_t stopAfter = 0;
  SaxParser* parser = nullptr;
};

void onStart(void* ud, const char*, const char**) {
  auto* s = static_cast<CountingState*>(ud);
  if (++s->elements >= s->stopAfter && s->stopAfter > 0) s->parser->stop();
}
void onEnd(void*, const char*) {}

struct StreamedParseFixture : testing::Test {
  fs::path work;
  std::shared_ptr<Epub> epub;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("streamed_parse_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    epub = std::make_shared<Epub>(kBook, (work / "cache").string());
    ASSERT_TRUE(epub->load(true));
    epub->setupCacheDir();
  }
  void TearDown() override {
    epub.reset();
    fs::remove_all(work);
  }
};

}  // namespace

TEST_F(StreamedParseFixture, AWholeChapterStreamsThroughWithoutATempFile) {
  CountingState state;
  SaxParser parser;
  ASSERT_TRUE(parser.init(&state, onStart, onEnd));
  state.parser = &parser;
  EXPECT_TRUE(ChapterXPathIndexerInternal::streamSpine(epub, kAppendixSpine, parser));
  EXPECT_GT(state.elements, 20u);
  EXPECT_FALSE(fs::exists(fs::path(epub->getCachePath()) / ".tmp_kox_12.html"));
}

TEST_F(StreamedParseFixture, AStoppedParserStopsTheInflate) {
  CountingState state;
  state.stopAfter = 3;
  SaxParser parser;
  ASSERT_TRUE(parser.init(&state, onStart, onEnd));
  state.parser = &parser;
  // Count the bytes the sink was handed: a sink that stops is not fed the whole chapter.
  struct CountingSink final : Print {
    SaxFeedSink inner;
    size_t bytes = 0;
    explicit CountingSink(SaxParser& p) : inner(p) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
      bytes += n;
      return inner.write(buf, n);
    }
  } sink(parser);
  const auto href = epub->getSpineItem(kAppendixSpine).href;
  EXPECT_TRUE(epub->readItemContentsToStream(href, sink, 1024, sink.inner.stopFlag()));
  EXPECT_TRUE(parser.isStopped());
  EXPECT_EQ(state.elements, 3u);
  EXPECT_LT(sink.bytes, 4096u) << "the inflate ran on after the parser stopped";
}

TEST_F(StreamedParseFixture, CountingStreamsToo) {
  const auto counted = ChapterXPathIndexerInternal::countTotalTextBytes(epub, kAppendixSpine);
  ASSERT_TRUE(counted.has_value());
  EXPECT_GT(*counted, 10000u);
  // A chapter that cannot be read is a failure, not a chapter with no text.
  EXPECT_FALSE(ChapterXPathIndexerInternal::countTotalTextBytes(epub, 99).has_value());
}

TEST_F(StreamedParseFixture, AnUnreadableChapterYieldsNoXPathRatherThanTheBaseOne) {
  // "" (uncached, so the next push retries) rather than the chapter-start xpath.
  EXPECT_EQ(ChapterXPathIndexer::findXPathForProgress(epub, 99, 0.5f), "");
  EXPECT_EQ(ChapterXPathIndexer::findXPathForProgress(epub, 99, 0.5f), "");
}
