#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "clippings/ClippingJumpValidation.h"

namespace {

struct FixtureData {
  ClippingCodec::BookMetadata book{"Title", "Author", "/books/title.epub", "epub"};
  std::string storePath = ClippingCodec::filePathForBook(book.path, book.bookType);
  std::vector<ClippingCodec::ClippingMetadata> entries;
  ClippingJumpResult request;

  FixtureData() {
    ClippingCodec::ClippingMetadata clipping;
    clipping.spineIndex = 3;
    clipping.startPage = 7;
    clipping.endPage = 7;
    clipping.pageCount = 20;
    clipping.startWordIndex = 4;
    clipping.endWordIndex = 11;
    clipping.wordCount = 8;
    clipping.paragraphIndex = 5;
    clipping.timestamp = 1700000000U;
    clipping.textOffset = 1024;
    clipping.textLength = 42;
    clipping.chapterTitle = "Chapter";
    clipping.pageFingerprint = 0xAABBCCDDU;
    entries.push_back(clipping);

    request.bookTitle = book.title;
    request.bookAuthor = book.author;
    request.bookPath = book.path;
    request.bookType = book.bookType;
    request.storePath = storePath;
    request.chapterTitle = clipping.chapterTitle;
    request.storeFileLength = 2048;
    request.storeFormat = static_cast<uint8_t>(ClippingCodec::Format::Current);
    request.clippingIndex = 0;
    request.spineIndex = clipping.spineIndex;
    request.startPage = clipping.startPage;
    request.endPage = clipping.endPage;
    request.pageCount = clipping.pageCount;
    request.startWordIndex = clipping.startWordIndex;
    request.endWordIndex = clipping.endWordIndex;
    request.wordCount = clipping.wordCount;
    request.paragraphIndex = clipping.paragraphIndex;
    request.timestamp = clipping.timestamp;
    request.textOffset = clipping.textOffset;
    request.textLength = clipping.textLength;
    request.textCrc32 = 0x12345678U;
    request.pageFingerprint = clipping.pageFingerprint;
  }

  ClippingJumpValidation::StoreSnapshot snapshot() const {
    return {book, storePath, ClippingCodec::Format::Current, 2048, entries, 8, 0x12345678U};
  }
};

TEST(ClippingJumpValidationTest, AcceptsOnlyTheCompleteExactCurrentEntry) {
  const FixtureData data;
  EXPECT_TRUE(ClippingJumpValidation::isExact(data.request, data.snapshot()));
}

TEST(ClippingJumpValidationTest, RejectsEveryBookStoreAndEntryIdentityMismatch) {
  using Mutation = std::pair<const char*, std::function<void(FixtureData&)>>;
  const std::vector<Mutation> mutations = {
      {"book title", [](auto& d) { d.request.bookTitle += " changed"; }},
      {"book author", [](auto& d) { d.request.bookAuthor += " changed"; }},
      {"book path", [](auto& d) { d.request.bookPath += ".other"; }},
      {"book type", [](auto& d) { d.request.bookType = "xtc"; }},
      {"store path", [](auto& d) { d.request.storePath += ".bak"; }},
      {"store length", [](auto& d) { ++d.request.storeFileLength; }},
      {"spine", [](auto& d) { ++d.request.spineIndex; }},
      {"start page", [](auto& d) { ++d.request.startPage; }},
      {"end page", [](auto& d) { ++d.request.endPage; }},
      {"page count", [](auto& d) { ++d.request.pageCount; }},
      {"start word", [](auto& d) { ++d.request.startWordIndex; }},
      {"end word", [](auto& d) { ++d.request.endWordIndex; }},
      {"word count", [](auto& d) { ++d.request.wordCount; }},
      {"paragraph", [](auto& d) { ++d.request.paragraphIndex; }},
      {"timestamp", [](auto& d) { ++d.request.timestamp; }},
      {"text offset", [](auto& d) { ++d.request.textOffset; }},
      {"text length", [](auto& d) { ++d.request.textLength; }},
      {"text crc", [](auto& d) { ++d.request.textCrc32; }},
      {"fingerprint", [](auto& d) { ++d.request.pageFingerprint; }},
      {"chapter title", [](auto& d) { d.request.chapterTitle += " changed"; }},
  };

  for (const auto& [name, mutate] : mutations) {
    SCOPED_TRACE(name);
    FixtureData changed;
    mutate(changed);
    EXPECT_FALSE(ClippingJumpValidation::isExact(changed.request, changed.snapshot()));
  }
}

TEST(ClippingJumpValidationTest, RejectsStaleIndexLegacyMultiPageAndMissingFingerprint) {
  FixtureData data;

  data.request.clippingIndex = 1;
  EXPECT_FALSE(ClippingJumpValidation::isExact(data.request, data.snapshot()));

  data = FixtureData{};
  data.request.storeFormat = static_cast<uint8_t>(ClippingCodec::Format::CrossInkV2);
  EXPECT_FALSE(ClippingJumpValidation::isExact(data.request, data.snapshot()));

  data = FixtureData{};
  data.entries[0].endPage = 8;
  data.request.endPage = 8;
  EXPECT_FALSE(ClippingJumpValidation::isExact(data.request, data.snapshot()));

  data = FixtureData{};
  data.entries[0].pageFingerprint = 0;
  data.request.pageFingerprint = 0;
  EXPECT_FALSE(ClippingJumpValidation::isExact(data.request, data.snapshot()));
}

TEST(ClippingJumpValidationTest, RejectsOutOfRangeSpineEvenWhenStoredMetadataMatches) {
  FixtureData data;
  data.entries[0].spineIndex = 8;
  data.request.spineIndex = 8;
  EXPECT_FALSE(ClippingJumpValidation::isExact(data.request, data.snapshot()));
}

}  // namespace
