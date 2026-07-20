#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ClipTextBuilder.h"
#include "ClippingCodec.h"

namespace {

using ClippingCodec::BookMetadata;
using ClippingCodec::ClippingMetadata;
using ClippingCodec::Format;
using ClippingCodec::Index;
using ClippingCodec::Source;
using ClippingCodec::Status;

void writeU16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void appendU16(std::vector<uint8_t>& bytes, const uint16_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + 2);
  writeU16(bytes, offset, value);
}

void appendU32(std::vector<uint8_t>& bytes, const uint32_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + 4);
  writeU32(bytes, offset, value);
}

void appendString32(std::vector<uint8_t>& bytes, const std::string& value) {
  appendU32(bytes, static_cast<uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

bool memoryReadAt(void* context, const uint32_t offset, uint8_t* out, const size_t length) {
  const auto& bytes = *static_cast<const std::vector<uint8_t>*>(context);
  if (offset > bytes.size() || length > bytes.size() - offset) return false;
  std::copy_n(bytes.data() + offset, length, out);
  return true;
}

Source sourceFor(const std::vector<uint8_t>& bytes) {
  return {const_cast<std::vector<uint8_t>*>(&bytes), static_cast<uint32_t>(bytes.size()), memoryReadAt};
}

ClippingMetadata sampleClipping() {
  ClippingMetadata clipping;
  clipping.spineIndex = 2;
  clipping.startPage = 3;
  clipping.endPage = 4;
  clipping.pageCount = 10;
  clipping.startWordIndex = 7;
  clipping.endWordIndex = 2;
  clipping.wordCount = 12;
  clipping.paragraphIndex = 9;
  clipping.timestamp = 123456;
  clipping.chapterTitle = "Chương một";
  clipping.pageFingerprint = 0x1234ABCDU;
  return clipping;
}

std::vector<uint8_t> makeCurrentFile(const BookMetadata& book,
                                     const std::vector<std::pair<ClippingMetadata, std::string>>& sourceClippings) {
  std::vector<uint8_t> metadata;
  EXPECT_EQ(ClippingCodec::encodeBookMetadata(book, metadata), Status::Ok);
  const uint32_t recordsOffset = static_cast<uint32_t>(ClippingCodec::HEADER_SIZE + metadata.size());
  const uint32_t textsOffset =
      recordsOffset + static_cast<uint32_t>(sourceClippings.size() * ClippingCodec::RECORD_SIZE);

  std::vector<std::array<uint8_t, ClippingCodec::RECORD_SIZE>> records(sourceClippings.size());
  uint32_t textOffset = textsOffset;
  for (size_t i = 0; i < sourceClippings.size(); ++i) {
    ClippingMetadata clipping = sourceClippings[i].first;
    clipping.textOffset = textOffset;
    clipping.textLength = static_cast<uint16_t>(sourceClippings[i].second.size());
    EXPECT_EQ(ClippingCodec::encodeRecord(clipping, records[i]), Status::Ok);
    textOffset += clipping.textLength;
  }

  std::vector<uint8_t> payload = metadata;
  for (const auto& record : records) payload.insert(payload.end(), record.begin(), record.end());
  for (const auto& clipping : sourceClippings) {
    payload.insert(payload.end(), clipping.second.begin(), clipping.second.end());
  }

  std::array<uint8_t, ClippingCodec::HEADER_SIZE> header{};
  ClippingCodec::encodeHeader(static_cast<uint16_t>(sourceClippings.size()),
                              static_cast<uint32_t>(ClippingCodec::HEADER_SIZE + payload.size()),
                              ClippingCodec::crc32(payload.data(), payload.size()),
                              static_cast<uint32_t>(metadata.size()), recordsOffset, textsOffset, header);
  std::vector<uint8_t> file(header.begin(), header.end());
  file.insert(file.end(), payload.begin(), payload.end());
  return file;
}

void refreshCurrentChecksum(std::vector<uint8_t>& file) {
  const uint32_t checksum =
      ClippingCodec::crc32(file.data() + ClippingCodec::HEADER_SIZE, file.size() - ClippingCodec::HEADER_SIZE);
  writeU32(file, 12, checksum);
}

std::vector<uint8_t> makeLegacyFile(const uint8_t version, const BookMetadata& book,
                                    const std::vector<std::pair<ClippingMetadata, std::string>>& clippings) {
  std::vector<uint8_t> file;
  file.push_back(version);
  appendU16(file, static_cast<uint16_t>(clippings.size()));
  appendString32(file, book.title);
  appendString32(file, book.author);
  appendString32(file, book.path);
  for (const auto& [clipping, text] : clippings) {
    appendU16(file, clipping.spineIndex);
    appendU16(file, clipping.startPage);
    appendU16(file, clipping.endPage);
    appendU16(file, clipping.pageCount);
    appendU16(file, clipping.startWordIndex);
    appendU16(file, clipping.endWordIndex);
    appendU16(file, clipping.wordCount);
    appendU16(file, clipping.paragraphIndex);
    appendU32(file, clipping.timestamp);
    std::array<uint8_t, 48> chapter{};
    std::copy_n(clipping.chapterTitle.begin(), std::min(clipping.chapterTitle.size(), chapter.size() - 1),
                chapter.begin());
    file.insert(file.end(), chapter.begin(), chapter.end());
    if (version == 1) {
      appendU32(file, static_cast<uint32_t>(text.size()));
    } else {
      appendU16(file, static_cast<uint16_t>(text.size()));
    }
    file.insert(file.end(), text.begin(), text.end());
  }
  return file;
}

BookMetadata sampleBook() { return {"Dế Mèn phiêu lưu ký", "Tô Hoài", "/Books/Tiếng Việt.epub", "epub"}; }

ClipTextBuilder::Word word(std::string text, const int x, const int y = 10) {
  ClipTextBuilder::Word result;
  result.x = x;
  result.y = y;
  result.width = static_cast<int>(text.size()) * 4;
  result.height = 10;
  result.text = std::move(text);
  return result;
}

std::vector<uint16_t> orderFor(const size_t count) {
  std::vector<uint16_t> order(count);
  for (size_t i = 0; i < count; ++i) order[i] = static_cast<uint16_t>(i);
  return order;
}

}  // namespace

TEST(ClippingCodec, UsesStandardCrc32AndDeterministicCrossInkPath) {
  constexpr char known[] = "123456789";
  EXPECT_EQ(ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(known), sizeof(known) - 1), 0xCBF43926U);
  EXPECT_EQ(ClippingCodec::filePathForBook("/Books/Tiếng Việt.epub"), "/.crosspoint/clippings/epub_1878542716.bin");
  EXPECT_TRUE(ClippingCodec::filePathForBook("/book.epub", "../epub").empty());
}

TEST(ClippingCodec, KeepsMetadataResidentAndReadsVietnameseTextOnDemand) {
  const std::string text = "Tiếng Việt: co\xCC\x81 dấu kết hợp.";
  const auto file = makeCurrentFile(sampleBook(), {{sampleClipping(), text}});
  Index index;
  ASSERT_EQ(ClippingCodec::inspect(sourceFor(file), index), Status::Ok);
  ASSERT_EQ(index.format, Format::Current);
  ASSERT_EQ(index.clippings.size(), 1U);
  EXPECT_EQ(index.book.title, "Dế Mèn phiêu lưu ký");
  EXPECT_EQ(index.clippings[0].chapterTitle, "Chương một");
  EXPECT_EQ(index.clippings[0].pageFingerprint, 0x1234ABCDU);
  EXPECT_EQ(index.clippings[0].textLength, text.size());

  std::string loadedText;
  EXPECT_EQ(ClippingCodec::readText(sourceFor(file), index.clippings[0], loadedText), Status::Ok);
  EXPECT_EQ(loadedText, text);
}

TEST(ClippingCodec, RejectsTruncationChecksumDamageAndInvalidOffsets) {
  const auto valid = makeCurrentFile(sampleBook(), {{sampleClipping(), "nội dung"}});
  for (size_t cut : {size_t{1}, ClippingCodec::HEADER_SIZE - 1, valid.size() - 1}) {
    std::vector<uint8_t> truncated(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(cut));
    Index index;
    EXPECT_NE(ClippingCodec::inspect(sourceFor(truncated), index), Status::Ok);
  }

  auto checksumDamage = valid;
  checksumDamage.back() ^= 0x01;
  Index index;
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(checksumDamage), index), Status::Corrupt);

  auto badOffset = valid;
  const uint32_t recordsOffset =
      static_cast<uint32_t>(ClippingCodec::HEADER_SIZE + 2 + sampleBook().title.size() + 2 +
                            sampleBook().author.size() + 2 + sampleBook().path.size() + 1 + 4);
  writeU32(badOffset, recordsOffset + 20, static_cast<uint32_t>(badOffset.size()));
  refreshCurrentChecksum(badOffset);
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(badOffset), index), Status::Corrupt);
}

TEST(ClippingCodec, RejectsNewerVersionsAndInvalidUtf8EvenWithValidChecksum) {
  auto newer = makeCurrentFile(sampleBook(), {{sampleClipping(), "text"}});
  writeU16(newer, 4, ClippingCodec::VERSION + 1);
  Index index;
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(newer), index), Status::NewerVersion);

  auto invalidUtf8 = makeCurrentFile(sampleBook(), {{sampleClipping(), "text"}});
  invalidUtf8.back() = 0xFF;
  refreshCurrentChecksum(invalidUtf8);
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(invalidUtf8), index), Status::InvalidUtf8);
}

TEST(ClippingCodec, EnforcesSixtyFourClippingAndTextLimits) {
  std::vector<std::pair<ClippingMetadata, std::string>> atLimit(ClippingCodec::MAX_CLIPPINGS_PER_BOOK,
                                                                {sampleClipping(), "x"});
  auto file = makeCurrentFile(sampleBook(), atLimit);
  Index index;
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(file), index), Status::Ok);
  EXPECT_EQ(index.clippings.size(), ClippingCodec::MAX_CLIPPINGS_PER_BOOK);

  atLimit.push_back({sampleClipping(), "x"});
  file = makeCurrentFile(sampleBook(), atLimit);
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(file), index), Status::LimitExceeded);

  ClippingMetadata tooLong = sampleClipping();
  tooLong.textLength = ClippingCodec::MAX_TEXT_BYTES + 1;
  std::array<uint8_t, ClippingCodec::RECORD_SIZE> record{};
  EXPECT_EQ(ClippingCodec::encodeRecord(tooLong, record), Status::LimitExceeded);
}

TEST(ClippingCodec, ReadsOnlyUnambiguousCrossInkV1AndV2Files) {
  for (const uint8_t version : {uint8_t{1}, uint8_t{2}}) {
    auto legacy = makeLegacyFile(version, sampleBook(), {{sampleClipping(), "bản cũ"}});
    Index index;
    ASSERT_EQ(ClippingCodec::inspect(sourceFor(legacy), index), Status::Ok);
    EXPECT_EQ(index.format, version == 1 ? Format::CrossInkV1 : Format::CrossInkV2);
    std::string text;
    ASSERT_EQ(ClippingCodec::readText(sourceFor(legacy), index.clippings[0], text), Status::Ok);
    EXPECT_EQ(text, "bản cũ");

    legacy.push_back(0x00);
    EXPECT_EQ(ClippingCodec::inspect(sourceFor(legacy), index), Status::Corrupt);
  }
}

TEST(ClippingCodec, RejectsMalformedLegacyLengthsAndMissingChapterTerminator) {
  auto oversizedCount = makeLegacyFile(2, sampleBook(), {});
  writeU16(oversizedCount, 1, ClippingCodec::MAX_CLIPPINGS_PER_BOOK + 1);
  Index index;
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(oversizedCount), index), Status::LimitExceeded);

  auto missingTerminator = makeLegacyFile(2, sampleBook(), {{sampleClipping(), "text"}});
  const size_t recordStart =
      3 + 4 + sampleBook().title.size() + 4 + sampleBook().author.size() + 4 + sampleBook().path.size();
  std::fill(missingTerminator.begin() + static_cast<std::ptrdiff_t>(recordStart + 20),
            missingTerminator.begin() + static_cast<std::ptrdiff_t>(recordStart + 68), 'A');
  EXPECT_EQ(ClippingCodec::inspect(sourceFor(missingTerminator), index), Status::Corrupt);
}

TEST(ClipTextBuilder, PreservesVietnameseAndCombiningMarks) {
  std::vector<ClipTextBuilder::Word> words = {word("Tiếng", 0), word("Viê\xCC\xA3t", 40), word("co\xCC\x81", 80)};
  ClipTextBuilder::Result result;
  ASSERT_EQ(ClipTextBuilder::build(words, orderFor(words.size()), 0, 2, 0, 1, result), ClipTextBuilder::Status::Ok);
  EXPECT_EQ(result.text, "Tiếng Viê\xCC\xA3t co\xCC\x81");
  EXPECT_EQ(result.wordCount, 3);
}

TEST(ClipTextBuilder, NormalizesNbspAndStartsANewParagraphAtEmSpace) {
  std::vector<ClipTextBuilder::Word> words = {word("Xin\xC2\xA0"
                                                   "chào",
                                                   0),
                                              word("\xE2\x80\x83"
                                                   "đoạn",
                                                   70),
                                              word("mới\xE2\x80\xAFnhé", 110)};
  ClipTextBuilder::Result result;
  ASSERT_EQ(ClipTextBuilder::build(words, orderFor(words.size()), 0, 2, 0, 1, result), ClipTextBuilder::Status::Ok);
  EXPECT_EQ(result.text, "Xin chào\nđoạn mới nhé");
}

TEST(ClipTextBuilder, AppliesPunctuationSpacingWithoutJoiningNormalWords) {
  std::vector<ClipTextBuilder::Word> words = {word("Xin", 0),  word(",", 20),  word("chào", 30), word("(", 60),
                                              word("bạn", 70), word(")", 100), word("!", 110)};
  ClipTextBuilder::Result result;
  ASSERT_EQ(ClipTextBuilder::build(words, orderFor(words.size()), 0, words.size() - 1, 0, 1, result),
            ClipTextBuilder::Status::Ok);
  EXPECT_EQ(result.text, "Xin, chào (bạn)!");
}

TEST(ClipTextBuilder, DistinguishesInsertedAndAuthoredHyphens) {
  auto inserted = word("tuyệt-", 0);
  inserted.endsWithInsertedHyphen = true;
  std::vector<ClipTextBuilder::Word> insertedWords = {inserted, word("vời", 0, 25)};
  ClipTextBuilder::Result result;
  ASSERT_EQ(ClipTextBuilder::build(insertedWords, orderFor(2), 0, 1, 0, 1, result), ClipTextBuilder::Status::Ok);
  EXPECT_EQ(result.text, "tuyệtvời");

  std::vector<ClipTextBuilder::Word> authoredWords = {word("hiện-", 0), word("đại", 0, 25)};
  ASSERT_EQ(ClipTextBuilder::build(authoredWords, orderFor(2), 0, 1, 0, 1, result), ClipTextBuilder::Status::Ok);
  EXPECT_EQ(result.text, "hiện-đại");
}

TEST(ClipTextBuilder, RejectsMalformedSelectionsAndTextBeyondStorageLimit) {
  ClipTextBuilder::Result result;
  std::vector<ClipTextBuilder::Word> words = {word(std::string(ClippingCodec::MAX_TEXT_BYTES, 'a'), 0)};
  EXPECT_EQ(ClipTextBuilder::build(words, orderFor(1), 0, 0, 0, 1, result), ClipTextBuilder::Status::Ok);
  words[0].text.push_back('a');
  EXPECT_EQ(ClipTextBuilder::build(words, orderFor(1), 0, 0, 0, 1, result), ClipTextBuilder::Status::TextTooLong);

  words = {word(std::string("\xC3", 1), 0)};
  EXPECT_EQ(ClipTextBuilder::build(words, orderFor(1), 0, 0, 0, 1, result), ClipTextBuilder::Status::InvalidUtf8);
  EXPECT_EQ(ClipTextBuilder::build(words, {2}, 0, 0, 0, 1, result), ClipTextBuilder::Status::InvalidSelection);
}
