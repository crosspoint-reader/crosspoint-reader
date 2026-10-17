// KOReaderDocumentId::calculate() is KOReader's partial MD5: 1024-byte samples at 0 and at
// 1024 << 2i for i = 0..10, as far as the file reaches. Any sample it cannot read in full makes
// the id one no other device computes -- and the id is cached next to the book, so a single bad
// read used to keep the book syncing under the wrong document until the file changed.
//
// The read used to land in a size_t, so HalFile::read()'s -1 became a SIZE_MAX hash length, and a
// failed seek skipped its sample. Both now fail the id, and a failed id is never cached.
// Based on crosspoint-reader PR #3425 (Daviex), whose tests cover the same failures.
#include <KOReaderDocumentId.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "MD5Builder.h"

namespace {

class KOReaderDocumentIdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    kodoc_fixture::state = {};
    kodoc_fixture::hashLog = {};
  }

  static void makeBook(const size_t size) {
    auto& book = kodoc_fixture::state.book;
    book.resize(size);
    for (size_t i = 0; i < size; ++i) book[i] = static_cast<uint8_t>((i * 37 + i / 251) & 0xFF);
  }

  // The bytes KOReader hashes for this book, worked out from the file independently.
  static std::vector<uint8_t> koreaderSamples() {
    const auto& book = kodoc_fixture::state.book;
    std::vector<uint8_t> samples;
    for (int i = -1; i <= 10; ++i) {
      const size_t offset = i < 0 ? 0 : static_cast<size_t>(1024) << (2 * i);
      if (offset >= book.size()) continue;
      const size_t len = std::min<size_t>(1024, book.size() - offset);
      samples.insert(samples.end(), book.begin() + offset, book.begin() + offset + len);
    }
    return samples;
  }

  static bool hashCached() {
    for (const auto& [path, content] : kodoc_fixture::state.files) {
      if (path.size() >= 18 && path.compare(path.size() - 18, 18, "koreader_docid.txt") == 0) return true;
    }
    return false;
  }

  // The id of the intact book, computed with no faults and then forgotten again.
  static std::string intactId() {
    const auto saved = kodoc_fixture::state;
    kodoc_fixture::state.failSeekTo = -1;
    kodoc_fixture::state.faultReadAt = -1;
    kodoc_fixture::state.files.clear();
    const std::string id = KOReaderDocumentId::calculate(kodoc_fixture::kBookPath);
    kodoc_fixture::state = saved;
    kodoc_fixture::hashLog = {};
    return id;
  }
};

TEST_F(KOReaderDocumentIdTest, HashesEveryKOReaderSampleOfAReadableBook) {
  // 1500 and 66000 end on a partial last sample, which is a complete read, not a short one.
  for (const size_t size : {size_t{1}, size_t{1023}, size_t{1024}, size_t{1500}, size_t{66000}}) {
    SCOPED_TRACE(size);
    SetUp();
    makeBook(size);
    const std::string id = KOReaderDocumentId::calculate(kodoc_fixture::kBookPath);
    EXPECT_EQ(id.size(), 32u);
    EXPECT_EQ(kodoc_fixture::hashLog.bytes, koreaderSamples());
    EXPECT_TRUE(hashCached());
    EXPECT_EQ(kodoc_fixture::state.openFiles, 0);
    EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), id) << "cache hit must return the same id";
  }
}

TEST_F(KOReaderDocumentIdTest, AFailedReadFailsTheIdInsteadOfHashingSizeMax) {
  makeBook(66000);
  const std::string intact = intactId();
  kodoc_fixture::state.faultReadAt = 4096;
  kodoc_fixture::state.faultReadResult = -1;

  EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), "");
  EXPECT_LE(kodoc_fixture::hashLog.largestAdd, 1024u) << "read()'s -1 reached md5.add() as a length";
  EXPECT_FALSE(hashCached()) << "a failed id must not be cached";
  EXPECT_EQ(kodoc_fixture::state.openFiles, 0);

  kodoc_fixture::state.faultReadAt = -1;
  EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), intact) << "the next try must hash the book";
}

TEST_F(KOReaderDocumentIdTest, AShortReadFailsTheId) {
  makeBook(66000);
  kodoc_fixture::state.faultReadAt = 1024;
  kodoc_fixture::state.faultReadResult = 17;

  EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), "");
  EXPECT_FALSE(hashCached());
  EXPECT_EQ(kodoc_fixture::state.openFiles, 0);
}

TEST_F(KOReaderDocumentIdTest, AFailedSeekFailsTheIdInsteadOfSkippingTheSample) {
  makeBook(66000);
  const std::string intact = intactId();
  kodoc_fixture::state.failSeekTo = 16384;

  EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), "");
  EXPECT_FALSE(hashCached()) << "a partial id must not be cached";
  EXPECT_EQ(kodoc_fixture::state.openFiles, 0);

  kodoc_fixture::state.failSeekTo = -1;
  EXPECT_EQ(KOReaderDocumentId::calculate(kodoc_fixture::kBookPath), intact);
}

TEST_F(KOReaderDocumentIdTest, AMissingBookHasNoId) {
  EXPECT_EQ(KOReaderDocumentId::calculate("/books/other.epub"), "");
  EXPECT_TRUE(kodoc_fixture::hashLog.bytes.empty());
  EXPECT_FALSE(hashCached());
}

}  // namespace
