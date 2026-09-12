#include <ChapterXPathResolver.h>
#include <expat.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

struct ParserAllocations {
  unsigned attempts = 0, created = 0, liveParsers = 0, peakParsers = 0;
  unsigned failAttempt = 0;
  bool failAllocation = false;
  std::unordered_map<void*, size_t> blocks;
  std::vector<size_t> blocksBeforeCreation;
};
ParserAllocations allocations;

void* parserMalloc(size_t size) {
  if (allocations.failAllocation) return nullptr;
  void* block = std::malloc(size);
  if (block) allocations.blocks[block] = size;
  return block;
}

void parserFree(void* block) {
  if (!block) return;
  EXPECT_EQ(allocations.blocks.erase(block), 1U);
  std::free(block);
}

void* parserRealloc(void* block, size_t size) {
  if (!block) return parserMalloc(size);
  if (size == 0) {
    parserFree(block);
    return nullptr;
  }
  if (allocations.failAllocation) return nullptr;
  const auto allocation = allocations.blocks.find(block);
  EXPECT_NE(allocation, allocations.blocks.end());
  void* resized = std::realloc(block, size);
  if (resized) {
    allocations.blocks.erase(allocation);
    allocations.blocks[resized] = size;
  }
  return resized;
}

class KOReaderParserLifetimeTest : public ::testing::Test {
 protected:
  void SetUp() override { allocations = {}; }
  void TearDown() override {
    EXPECT_EQ(allocations.liveParsers, 0U);
    EXPECT_TRUE(allocations.blocks.empty());
  }
  static std::shared_ptr<Epub> book(const std::string& body, size_t chunk = 0) {
    return std::make_shared<Epub>("<html><body>" + body + "</body></html>", chunk);
  }
};

TEST_F(KOReaderParserLifetimeTest, CountingParserIsFreedBeforeTheResolvingPassAllocates) {
  auto epub = book("<p>alpha</p><p>bravo</p>");
  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.75f), "/body/DocFragment[1]/body/p[2]/text()[1].3");
  EXPECT_EQ(epub->reads, 2U);
  EXPECT_EQ(allocations.created, 2U);
  EXPECT_EQ(allocations.peakParsers, 1U);
  ASSERT_EQ(allocations.blocksBeforeCreation.size(), 2U);
  EXPECT_EQ(allocations.blocksBeforeCreation[1], 0U);
}

TEST_F(KOReaderParserLifetimeTest, PercentageFallbackGoldensAreUnchangedAcrossStreamingChunks) {
  for (const size_t chunk : {1U, 7U, 1024U}) {
    SCOPED_TRACE(chunk);
    auto epub = book("<p>alpha</p><p>bravo</p>", chunk);
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.25f), "/body/DocFragment[1]/body/p[1]/text()[1].3");
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f), "/body/DocFragment[1]/body/p[1]/text()[1].5");
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[2]/text()[1].5");
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.5f), "/body/DocFragment[1]/body/p[2]/text()[1].5");
  }
}

TEST_F(KOReaderParserLifetimeTest, NestedUnicodeAndEntitiesKeepTheirExistingAnchor) {
  for (const size_t chunk : {1U, 5U, 1024U}) {
    auto epub = book("<section><p>A\u00e9&amp;<em>BC</em>D</p></section>", chunk);
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.75f),
              "/body/DocFragment[1]/body/section[1]/p[1]/em[1]/text()[1].2");
  }
}

TEST_F(KOReaderParserLifetimeTest, ChapterStartAndInvalidInputsDoNotAllocateParsers) {
  auto epub = book("<p>alpha</p>");
  for (const float progress : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, progress), "/body/DocFragment[1]/body");
  }
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(nullptr, 0, 0.5f).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(epub, -1, 0.5f).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(epub, 1, 0.5f).empty());
  EXPECT_EQ(allocations.attempts, 0U);
}

TEST_F(KOReaderParserLifetimeTest, EmptyVisibleTextSkipsTheResolvingPass) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(book("<div>outside paragraphs</div>"), 0, 0.5f).empty());
  EXPECT_EQ(allocations.created, 1U);
}

TEST_F(KOReaderParserLifetimeTest, MalformedInputReleasesTheCounterAndKeepsEmptyFallback) {
  auto epub = std::make_shared<Epub>("<html><body><p>unterminated");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f).empty());
  EXPECT_EQ(allocations.created, 1U);
}

TEST_F(KOReaderParserLifetimeTest, FailedCountingReadReleasesTheParser) {
  auto epub = book("<p>alpha</p>");
  epub->failRead = 1;
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f).empty());
  EXPECT_EQ(allocations.created, 1U);
}

TEST_F(KOReaderParserLifetimeTest, FailedResolvingReadHasNoCountingParserStillAlive) {
  auto epub = book("<p>alpha</p>");
  epub->failRead = 2;
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f).empty());
  EXPECT_EQ(allocations.created, 2U);
  EXPECT_EQ(allocations.peakParsers, 1U);
}

TEST_F(KOReaderParserLifetimeTest, CountingParserAllocationFailureKeepsEmptyFallback) {
  allocations.failAttempt = 1;
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(book("<p>alpha</p>"), 0, 0.5f).empty());
  EXPECT_EQ(allocations.attempts, 1U);
  EXPECT_EQ(allocations.created, 0U);
}

TEST_F(KOReaderParserLifetimeTest, ResolvingParserAllocationFailureHasAlreadyReleasedCounter) {
  allocations.failAttempt = 2;
  EXPECT_TRUE(ChapterXPathResolver::findXPathForProgress(book("<p>alpha</p>"), 0, 0.5f).empty());
  EXPECT_EQ(allocations.attempts, 2U);
  EXPECT_EQ(allocations.created, 1U);
  ASSERT_EQ(allocations.blocksBeforeCreation.size(), 2U);
  EXPECT_EQ(allocations.blocksBeforeCreation[1], 0U);
}

}  // namespace

extern "C" XML_Parser trackedXmlParserCreate(const XML_Char* encoding) {
  ++allocations.attempts;
  allocations.blocksBeforeCreation.push_back(allocations.blocks.size());
  allocations.failAllocation = allocations.attempts == allocations.failAttempt;
  const XML_Memory_Handling_Suite memory{parserMalloc, parserRealloc, parserFree};
  XML_Parser parser = XML_ParserCreate_MM(encoding, &memory, nullptr);
  allocations.failAllocation = false;
  if (parser) {
    ++allocations.created;
    ++allocations.liveParsers;
    allocations.peakParsers = std::max(allocations.peakParsers, allocations.liveParsers);
  }
  return parser;
}

extern "C" void trackedXmlParserFree(XML_Parser parser) {
  if (!parser) return;
  EXPECT_GT(allocations.liveParsers, 0U);
  XML_ParserFree(parser);
  --allocations.liveParsers;
}
