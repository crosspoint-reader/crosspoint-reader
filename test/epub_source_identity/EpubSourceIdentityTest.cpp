#include <HalStorage.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "Epub/BookMetadataCache.h"
#include "Epub/SourceIdentityCodec.h"
#include "Epub/SourceIdentityStore.h"

namespace {

constexpr char EPUB_PATH[] = "/books/test.epub";
constexpr char CACHE_PATH[] = "/.crosspoint/epub_test";
constexpr char BOOK_CACHE_PATH[] = "/.crosspoint/epub_test/book.bin";

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, const T value) {
  const auto* raw = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

template <typename T>
void overwritePod(std::vector<uint8_t>& bytes, const size_t offset, const T value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendString(std::vector<uint8_t>& bytes, const std::string& value) {
  appendPod(bytes, static_cast<uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void put16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void put32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> makeZip(const uint32_t entryCrc = 0x12345678U, const char nameByte = 'a') {
  std::vector<uint8_t> bytes(32, 0x5AU);
  const uint32_t centralOffset = static_cast<uint32_t>(bytes.size());
  constexpr uint16_t nameLength = 5;
  const size_t centralStart = bytes.size();
  bytes.resize(bytes.size() + 46 + nameLength, 0);
  put32(bytes, centralStart, 0x02014B50U);
  put32(bytes, centralStart + 16, entryCrc);
  put32(bytes, centralStart + 20, 12);
  put32(bytes, centralStart + 24, 20);
  put16(bytes, centralStart + 28, nameLength);
  bytes[centralStart + 46] = static_cast<uint8_t>(nameByte);
  bytes[centralStart + 47] = '.';
  bytes[centralStart + 48] = 'x';
  bytes[centralStart + 49] = 'h';
  bytes[centralStart + 50] = 't';
  const uint32_t centralSize = static_cast<uint32_t>(bytes.size() - centralStart);

  const size_t eocd = bytes.size();
  bytes.resize(bytes.size() + 22, 0);
  put32(bytes, eocd, 0x06054B50U);
  put16(bytes, eocd + 8, 1);
  put16(bytes, eocd + 10, 1);
  put32(bytes, eocd + 12, centralSize);
  put32(bytes, eocd + 16, centralOffset);
  return bytes;
}

void appendLe16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendLe32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

struct StoredZipEntry {
  std::string name;
  std::string contents;
  uint32_t localOffset = 0;
  uint32_t crc = 0;
};

std::vector<uint8_t> makeCssTestEpub(std::string stylesheet = ".note { text-align: right; margin-left: 2px; }") {
  std::vector<StoredZipEntry> entries = {
      {"META-INF/container.xml",
       R"(<?xml version="1.0"?><container><rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
      {"OPS/content.opf",
       R"(<?xml version="1.0"?><package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>CSS test</dc:title><dc:creator>CrossVi</dc:creator><dc:language>en</dc:language></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/><item id="style" href="styles.css" media-type="text/css"/></manifest><spine><itemref idref="chapter"/></spine></package>)"},
      {"OPS/chapter.xhtml", "<html><body><p class=\"note\">Test</p></body></html>"},
      {"OPS/styles.css", std::move(stylesheet)},
  };

  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < entries.size(); ++i) {
    auto& entry = entries[i];
    entry.localOffset = static_cast<uint32_t>(bytes.size());
    entry.crc = 0x13572468U + static_cast<uint32_t>(i);
    appendLe32(bytes, 0x04034B50U);
    appendLe16(bytes, 20);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);  // stored
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, entry.crc);
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe16(bytes, static_cast<uint16_t>(entry.name.size()));
    appendLe16(bytes, 0);
    bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
    bytes.insert(bytes.end(), entry.contents.begin(), entry.contents.end());
  }

  const uint32_t centralOffset = static_cast<uint32_t>(bytes.size());
  for (const auto& entry : entries) {
    appendLe32(bytes, 0x02014B50U);
    appendLe16(bytes, 20);
    appendLe16(bytes, 20);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);  // stored
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, entry.crc);
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe32(bytes, static_cast<uint32_t>(entry.contents.size()));
    appendLe16(bytes, static_cast<uint16_t>(entry.name.size()));
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe16(bytes, 0);
    appendLe32(bytes, 0);
    appendLe32(bytes, entry.localOffset);
    bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
  }

  const uint32_t centralSize = static_cast<uint32_t>(bytes.size()) - centralOffset;
  appendLe32(bytes, 0x06054B50U);
  appendLe16(bytes, 0);
  appendLe16(bytes, 0);
  appendLe16(bytes, static_cast<uint16_t>(entries.size()));
  appendLe16(bytes, static_cast<uint16_t>(entries.size()));
  appendLe32(bytes, centralSize);
  appendLe32(bytes, centralOffset);
  appendLe16(bytes, 0);
  return bytes;
}

ZipFile::SourceIdentity identify(const std::vector<uint8_t>& bytes) {
  Storage.setFile(EPUB_PATH, bytes);
  const std::string path = EPUB_PATH;
  ZipFile zip(path);
  ZipFile::SourceIdentity identity;
  EXPECT_TRUE(zip.getSourceIdentity(identity));
  return identity;
}

std::vector<uint8_t> makeBookCache(const ZipFile::SourceIdentity& identity) {
  constexpr uint8_t version = 10;
  constexpr uint16_t spineCount = 1;
  constexpr uint16_t tocCount = 1;
  constexpr uint32_t commitMarker = 0x424D434B;

  SourceIdentityCodec::Payload identityPayload{};
  EXPECT_TRUE(SourceIdentityCodec::encodePayload(identity, identityPayload));

  std::vector<uint8_t> bytes;
  appendPod(bytes, version);
  const size_t lutOffsetPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  appendPod(bytes, spineCount);
  appendPod(bytes, tocCount);
  bytes.insert(bytes.end(), identityPayload.begin(), identityPayload.end());
  appendPod(bytes, SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()));
  appendString(bytes, "A safe title");
  appendString(bytes, "An author");
  appendString(bytes, "en");
  appendString(bytes, "cover.xhtml");
  appendString(bytes, "text.xhtml");

  const uint32_t lutOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, lutOffsetPosition, lutOffset);
  const size_t spineLutPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  const size_t tocLutPosition = bytes.size();
  appendPod(bytes, uint32_t{0});

  const uint32_t spineOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, spineLutPosition, spineOffset);
  appendString(bytes, "OPS/chapter.xhtml");
  appendPod(bytes, uint32_t{1234});
  appendPod(bytes, int16_t{0});

  const uint32_t tocOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, tocLutPosition, tocOffset);
  appendString(bytes, "Chapter 1");
  appendString(bytes, "OPS/chapter.xhtml");
  appendString(bytes, "start");
  appendPod(bytes, uint8_t{1});
  appendPod(bytes, int16_t{0});
  appendPod(bytes, commitMarker);
  return bytes;
}

std::vector<uint8_t> makeSpineOnlyBookCache(const ZipFile::SourceIdentity& identity, const uint16_t spineCount) {
  constexpr uint8_t version = 10;
  constexpr uint32_t commitMarker = 0x424D434B;
  SourceIdentityCodec::Payload identityPayload{};
  EXPECT_TRUE(SourceIdentityCodec::encodePayload(identity, identityPayload));

  std::vector<uint8_t> bytes;
  appendPod(bytes, version);
  const size_t lutOffsetPosition = bytes.size();
  appendPod(bytes, uint32_t{0});
  appendPod(bytes, spineCount);
  appendPod(bytes, uint16_t{0});
  bytes.insert(bytes.end(), identityPayload.begin(), identityPayload.end());
  appendPod(bytes, SourceIdentityCodec::crc32(identityPayload.data(), identityPayload.size()));
  for (const std::string value : {"Book", "Author", "en", "", ""}) appendString(bytes, value);

  const uint32_t lutOffset = static_cast<uint32_t>(bytes.size());
  overwritePod(bytes, lutOffsetPosition, lutOffset);
  std::vector<size_t> lutPositions;
  lutPositions.reserve(spineCount);
  for (uint16_t index = 0; index < spineCount; ++index) {
    lutPositions.push_back(bytes.size());
    appendPod(bytes, uint32_t{0});
  }
  for (uint16_t index = 0; index < spineCount; ++index) {
    overwritePod(bytes, lutPositions[index], static_cast<uint32_t>(bytes.size()));
    appendString(bytes, "chapter-" + std::to_string(index));
    appendPod(bytes, static_cast<uint32_t>(index + 1));
    appendPod(bytes, int16_t{-1});
  }
  appendPod(bytes, commitMarker);
  return bytes;
}

class EpubSourceIdentityTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
  }
};

TEST_F(EpubSourceIdentityTest, CentralDirectoryIdentityIsStableAndContentSensitive) {
  const auto first = identify(makeZip());
  const auto again = identify(makeZip());
  const auto changedCrc = identify(makeZip(0x12345679U));
  const auto changedName = identify(makeZip(0x12345678U, 'b'));

  EXPECT_EQ(first, again);
  EXPECT_EQ(first.fileSize, changedCrc.fileSize);
  EXPECT_NE(first, changedCrc);
  EXPECT_NE(first, changedName);
  EXPECT_LE(Storage.maxRead(), 1024U);
}

TEST_F(EpubSourceIdentityTest, RejectsMalformedBoundsAndConcurrentGrowth) {
  auto malformed = makeZip();
  put32(malformed, malformed.size() - 22 + 12, UINT32_MAX);
  Storage.setFile(EPUB_PATH, malformed);
  const std::string path = EPUB_PATH;
  ZipFile malformedZip(path);
  ZipFile::SourceIdentity identity;
  EXPECT_FALSE(malformedZip.getSourceIdentity(identity));

  Storage.reset();
  Storage.setFile(EPUB_PATH, makeZip());
  Storage.growOnReadCall(2);
  ZipFile changingZip(path);
  EXPECT_FALSE(changingZip.getSourceIdentity(identity));
}

TEST_F(EpubSourceIdentityTest, CodecCrcRejectsIdentityBitFlipInsteadOfCallingItMismatch) {
  const auto identity = identify(makeZip());
  SourceIdentityCodec::Encoded encoded;
  ASSERT_TRUE(SourceIdentityCodec::encode(identity, encoded));

  ZipFile::SourceIdentity decoded;
  ASSERT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::OK);
  EXPECT_EQ(decoded, identity);

  encoded[SourceIdentityCodec::PAYLOAD_OFFSET + 3] ^= 0x01U;
  EXPECT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::BAD_CRC);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidatesAndReadsEveryEntry) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeBookCache(identity));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  EXPECT_EQ(cache.coreMetadata.title, "A safe title");
  EXPECT_EQ(cache.getSpineCount(), 1);
  EXPECT_EQ(cache.getTocCount(), 1);
  EXPECT_EQ(cache.getSpineEntry(0).href, "OPS/chapter.xhtml");
  EXPECT_EQ(cache.getSpineEntry(0).cumulativeSize, 1234U);
  EXPECT_EQ(cache.getTocEntry(0).title, "Chapter 1");
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsUnboundedMetadataBeforeAllocation) {
  const auto identity = identify(makeZip());
  auto bytes = makeBookCache(identity);
  constexpr size_t fixedHeaderSize = 1 + 4 + 2 + 2 + SourceIdentityCodec::PAYLOAD_SIZE + 4;
  overwritePod(bytes, fixedHeaderSize, UINT32_MAX);
  Storage.setFile(BOOK_CACHE_PATH, std::move(bytes));

  BookMetadataCache cache(CACHE_PATH);
  EXPECT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Invalid);
  EXPECT_LE(Storage.maxRead(), 128U);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsCorruptLutAndEntryLengths) {
  const auto identity = identify(makeZip());
  auto badLut = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, badLut.data() + 1, sizeof(lutOffset));
  overwritePod(badLut, lutOffset, uint32_t{0});
  Storage.setFile(BOOK_CACHE_PATH, std::move(badLut));
  BookMetadataCache badLutCache(CACHE_PATH);
  EXPECT_EQ(badLutCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto badLength = makeBookCache(identity);
  memcpy(&lutOffset, badLength.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, badLength.data() + lutOffset, sizeof(spineOffset));
  overwritePod(badLength, spineOffset, UINT32_MAX);
  Storage.setFile(BOOK_CACHE_PATH, std::move(badLength));
  BookMetadataCache badLengthCache(CACHE_PATH);
  EXPECT_EQ(badLengthCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheRejectsInvalidEntryReferencesAndOrdering) {
  const auto identity = identify(makeZip());
  auto badReference = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, badReference.data() + 1, sizeof(lutOffset));
  const size_t spineIndexOffset = badReference.size() - sizeof(uint32_t) - sizeof(int16_t);
  ASSERT_LT(spineIndexOffset, badReference.size());
  overwritePod(badReference, spineIndexOffset, int16_t{2});
  Storage.setFile(BOOK_CACHE_PATH, std::move(badReference));
  BookMetadataCache badReferenceCache(CACHE_PATH);
  EXPECT_EQ(badReferenceCache.load(identity), BookMetadataCache::LoadStatus::Invalid);

  auto overlapping = makeBookCache(identity);
  memcpy(&lutOffset, overlapping.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, overlapping.data() + lutOffset, sizeof(spineOffset));
  overwritePod(overlapping, lutOffset + sizeof(uint32_t), spineOffset);
  Storage.setFile(BOOK_CACHE_PATH, std::move(overlapping));
  BookMetadataCache overlappingCache(CACHE_PATH);
  EXPECT_EQ(overlappingCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, BookMetadataGetterFailsClosedIfFileChangesAfterLoad) {
  const auto identity = identify(makeZip());
  auto bytes = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, bytes.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, bytes.data() + lutOffset, sizeof(spineOffset));
  Storage.setFile(BOOK_CACHE_PATH, std::move(bytes));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  overwritePod(Storage.mutableFile(BOOK_CACHE_PATH), spineOffset, UINT32_MAX);
  EXPECT_TRUE(cache.getSpineEntry(0).href.empty());
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_EQ(cache.getLastLoadStatus(), BookMetadataCache::LoadStatus::Invalid);
}

TEST_F(EpubSourceIdentityTest, CheckedProgressNeverInventsZeroAfterMetadataReadFailure) {
  const auto identity = identify(makeZip());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  auto bookCache = makeBookCache(identity);
  uint32_t lutOffset = 0;
  memcpy(&lutOffset, bookCache.data() + 1, sizeof(lutOffset));
  uint32_t spineOffset = 0;
  memcpy(&spineOffset, bookCache.data() + lutOffset, sizeof(spineOffset));
  Storage.setFile(cachePath + "/book.bin", std::move(bookCache));
  ASSERT_TRUE(epub.load(false, true));

  float progress = 0.0F;
  ASSERT_TRUE(epub.calculateProgressChecked(0, 0.5F, progress));
  EXPECT_FLOAT_EQ(progress, 0.5F);

  overwritePod(Storage.mutableFile(cachePath + "/book.bin"), spineOffset, UINT32_MAX);
  progress = 0.75F;
  EXPECT_FALSE(epub.calculateProgressChecked(0, 0.5F, progress));
  EXPECT_FLOAT_EQ(progress, 0.0F);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheDistinguishesNewerFromCorruptWithoutDeletingEither) {
  const auto identity = identify(makeZip());
  auto newer = makeBookCache(identity);
  newer.front() = 11;
  Storage.setFile(BOOK_CACHE_PATH, newer);
  BookMetadataCache newerCache(CACHE_PATH);
  EXPECT_EQ(newerCache.load(identity), BookMetadataCache::LoadStatus::NewerVersion);
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), newer);

  std::vector<uint8_t> truncated = {10, 0, 0};
  Storage.setFile(BOOK_CACHE_PATH, truncated);
  BookMetadataCache truncatedCache(CACHE_PATH);
  EXPECT_EQ(truncatedCache.load(identity), BookMetadataCache::LoadStatus::Invalid);
  EXPECT_EQ(Storage.file(BOOK_CACHE_PATH), truncated);
}

TEST_F(EpubSourceIdentityTest, BookMetadataCacheValidatesAcrossBoundedLutChunks) {
  const auto identity = identify(makeZip());
  Storage.setFile(BOOK_CACHE_PATH, makeSpineOnlyBookCache(identity, 65));

  BookMetadataCache cache(CACHE_PATH);
  ASSERT_EQ(cache.load(identity), BookMetadataCache::LoadStatus::Loaded);
  EXPECT_EQ(cache.getSpineCount(), 65);
  EXPECT_EQ(cache.getSpineEntry(0).href, "chapter-0");
  EXPECT_EQ(cache.getSpineEntry(64).href, "chapter-64");
  EXPECT_LE(Storage.maxRead(), 65U * sizeof(uint32_t));
}

TEST_F(EpubSourceIdentityTest, CssCacheBuildIsVerifiedAndIdempotent) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  const std::string sectionsPath = cachePath + "/sections";
  const std::string sectionSentinel = sectionsPath + "/section_0.bin";
  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionSentinel, {0xAA});

  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(sectionSentinel.c_str()));
  const auto& firstCssCache = Storage.file(cachePath + "/css_rules.cache");
  ASSERT_GE(firstCssCache.size(), 3U);
  EXPECT_GT(static_cast<uint16_t>(firstCssCache[1]) | (static_cast<uint16_t>(firstCssCache[2]) << 8U), 0U);
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  const CssStyle rebuiltStyle = epub.getCssParser()->resolveStyle("p", "note");
  EXPECT_TRUE(rebuiltStyle.defined.textAlign);
  EXPECT_EQ(rebuiltStyle.textAlign, CssTextAlign::Right);
  epub.getCssParser()->clear();

  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionSentinel, {0xBB});
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_TRUE(Storage.exists(sectionSentinel.c_str()));

  auto& cssCache = Storage.mutableFile(cachePath + "/css_rules.cache");
  ASSERT_FALSE(cssCache.empty());
  cssCache.front() ^= 0xFFU;
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(sectionSentinel.c_str()));
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, WarmCssDiscoveryDoesNotTouchManifestScratchFile) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string scratchPath = cachePath + "/.items.bin";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  Storage.makeUnwritable(scratchPath);
  Storage.makeUnreadable(scratchPath);
  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_EQ(Storage.openWriteAttemptsFor(scratchPath), 0U);
  EXPECT_EQ(Storage.openReadAttemptsFor(scratchPath), 0U);
  EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  EXPECT_FALSE(Storage.exists(scratchPath.c_str()));

  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, ContentOpfItemScratchIoFaultsFailClosedWithoutInvalidHandleAccess) {
  enum class Fault { OpenWrite, ShortWrite, CloseWriter, OpenRead, ShortRead };
  for (const Fault fault :
       {Fault::OpenWrite, Fault::ShortWrite, Fault::CloseWriter, Fault::OpenRead, Fault::ShortRead}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    ASSERT_TRUE(Storage.mkdir(CACHE_PATH));
    identify(makeCssTestEpub());

    Epub epub(EPUB_PATH, "/.crosspoint");
    const std::string cachePath = epub.getCachePath();
    const std::string scratchPath = cachePath + "/.items.bin";
    ASSERT_TRUE(epub.bindCurrentSource());
    switch (fault) {
      case Fault::OpenWrite:
        Storage.makeUnwritable(scratchPath);
        break;
      case Fault::ShortWrite:
        Storage.shortWriteFor(scratchPath);
        break;
      case Fault::CloseWriter:
        Storage.failCloseFor(scratchPath);
        break;
      case Fault::OpenRead:
        Storage.makeUnreadable(scratchPath);
        break;
      case Fault::ShortRead:
        Storage.shortReadFor(scratchPath);
        break;
    }

    EXPECT_FALSE(epub.load(true, true));
    EXPECT_FALSE(Storage.exists((cachePath + "/book.bin").c_str()));
    EXPECT_FALSE(Storage.exists(scratchPath.c_str()));
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(EpubSourceIdentityTest, CssCachePublishFaultsLeaveNoCommittedOrTemporaryCache) {
  enum class Fault { OpenWrite, ShortWrite, Sync, Close, Rename, CorruptRename };
  for (const Fault fault :
       {Fault::OpenWrite, Fault::ShortWrite, Fault::Sync, Fault::Close, Fault::Rename, Fault::CorruptRename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint"));
    ASSERT_TRUE(Storage.mkdir(CACHE_PATH));
    const auto identity = identify(makeCssTestEpub());
    Epub epub(EPUB_PATH, "/.crosspoint");
    const std::string cachePath = epub.getCachePath();
    const std::string canonicalPath = cachePath + "/css_rules.cache";
    const std::string temporaryPath = cachePath + "/css_rules.cache.tmp";
    ASSERT_TRUE(epub.bindCurrentSource());
    Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
    ASSERT_TRUE(epub.load(false, true));

    switch (fault) {
      case Fault::OpenWrite:
        Storage.makeUnwritable(temporaryPath);
        break;
      case Fault::ShortWrite:
        Storage.shortWriteFor(temporaryPath);
        break;
      case Fault::Sync:
        Storage.failSyncOnce();
        break;
      case Fault::Close:
        Storage.failCloseFor(temporaryPath);
        break;
      case Fault::Rename:
        Storage.failRenameOnce();
        break;
      case Fault::CorruptRename:
        Storage.corruptRenameOnce();
        break;
    }

    EXPECT_FALSE(epub.ensureCssCache());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
    EXPECT_FALSE(Storage.exists(temporaryPath.c_str()));
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_EQ(epub.getTitle(), "A safe title");
    EXPECT_EQ(Storage.invalidOperationCount(), 0U);
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheRejectsBitFlipTruncationAndTrailingBytes) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_GT(validCache.size(), 8U);

  for (int mutation = 0; mutation < 3; ++mutation) {
    SCOPED_TRACE(mutation);
    auto corrupted = validCache;
    if (mutation == 0) corrupted[corrupted.size() - sizeof(uint32_t) - 1U] ^= 0x40U;
    if (mutation == 1) corrupted.pop_back();
    if (mutation == 2) corrupted.push_back(0xA5U);
    Storage.setFile(canonicalPath, std::move(corrupted));
    EXPECT_FALSE(epub.getCssParser()->loadFromCache());
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheRejectsSemanticGarbageWithValidCrc) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_GT(validCache.size(), 70U);

  uint16_t selectorLength = 0;
  memcpy(&selectorLength, validCache.data() + 3, sizeof(selectorLength));
  const size_t styleOffset = 5U + selectorLength;
  ASSERT_LE(styleOffset + 66U, validCache.size());

  for (int mutation = 0; mutation < 4; ++mutation) {
    SCOPED_TRACE(mutation);
    auto corrupted = validCache;
    if (mutation == 0) corrupted[styleOffset] = 0xFFU;
    if (mutation == 1) {
      const float nan = std::numeric_limits<float>::quiet_NaN();
      memcpy(corrupted.data() + styleOffset + 5U, &nan, sizeof(nan));
    }
    if (mutation == 2) corrupted[styleOffset + 9U] = 0xFFU;
    if (mutation == 3) {
      uint32_t definedBits = 0;
      memcpy(&definedBits, corrupted.data() + styleOffset + 62U, sizeof(definedBits));
      definedBits |= 1U << 18U;
      memcpy(corrupted.data() + styleOffset + 62U, &definedBits, sizeof(definedBits));
    }
    const uint32_t crc = SourceIdentityCodec::crc32(corrupted.data(), corrupted.size() - sizeof(uint32_t));
    memcpy(corrupted.data() + corrupted.size() - sizeof(uint32_t), &crc, sizeof(crc));
    Storage.setFile(canonicalPath, std::move(corrupted));
    EXPECT_FALSE(epub.getCssParser()->loadFromCache());
    EXPECT_TRUE(epub.getCssParser()->empty());
    EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  }
}

TEST_F(EpubSourceIdentityTest, CssCacheTempOnlyRecoveryRebuildsVerifiedCache) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  const std::string canonicalPath = cachePath + "/css_rules.cache";
  const std::string temporaryPath = cachePath + "/css_rules.cache.tmp";
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));
  ASSERT_TRUE(epub.ensureCssCache());
  const auto validCache = Storage.file(canonicalPath);
  ASSERT_TRUE(Storage.remove(canonicalPath.c_str()));
  Storage.setFile(temporaryPath, validCache);

  ASSERT_TRUE(epub.ensureCssCache());
  EXPECT_TRUE(Storage.exists(canonicalPath.c_str()));
  EXPECT_FALSE(Storage.exists(temporaryPath.c_str()));
  ASSERT_TRUE(epub.getCssParser()->loadFromCache());
  EXPECT_EQ(epub.getCssParser()->resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(EpubSourceIdentityTest, CssLengthToPixelsInt16ClampsNonFiniteAndRange) {
  EXPECT_EQ(CssLength(std::numeric_limits<float>::quiet_NaN()).toPixelsInt16(1), 0);
  EXPECT_EQ(CssLength(std::numeric_limits<float>::infinity()).toPixelsInt16(1), std::numeric_limits<int16_t>::max());
  EXPECT_EQ(CssLength(-std::numeric_limits<float>::infinity()).toPixelsInt16(1), std::numeric_limits<int16_t>::min());
  EXPECT_EQ(CssLength(100000.0F).toPixelsInt16(1), std::numeric_limits<int16_t>::max());
  EXPECT_EQ(CssLength(-100000.0F).toPixelsInt16(1), std::numeric_limits<int16_t>::min());
}

TEST_F(EpubSourceIdentityTest, OversizedStylesheetCannotPublishAPartialCssCache) {
  constexpr size_t MAX_SUPPORTED_CSS_SIZE = 128U * 1024U;
  const auto identity = identify(makeCssTestEpub(std::string(MAX_SUPPORTED_CSS_SIZE + 1U, 'x')));
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  EXPECT_FALSE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists((cachePath + "/css_rules.cache").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/.tmp.css").c_str()));
  EXPECT_EQ(epub.getTitle(), "A safe title");
}

TEST_F(EpubSourceIdentityTest, CssCacheReadBackFailureFailsClosedAndKeepsBookMetadataLoaded) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  ASSERT_TRUE(epub.load(false, true));

  const std::string cssCachePath = cachePath + "/css_rules.cache";
  Storage.makeUnreadable(cssCachePath);
  EXPECT_FALSE(epub.ensureCssCache());
  EXPECT_FALSE(Storage.exists(cssCachePath.c_str()));
  EXPECT_EQ(epub.getTitle(), "A safe title");
}

TEST_F(EpubSourceIdentityTest, BookLoadDegradesSafelyWhenCssCacheCannotBeVerified) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  Storage.makeUnreadable(cachePath + "/css_rules.cache");

  EXPECT_TRUE(epub.load(false, false));
  EXPECT_EQ(epub.getTitle(), "A safe title");
  EXPECT_TRUE(epub.isExternalCssUnavailable());
  EXPECT_FALSE(Storage.exists((cachePath + "/css_rules.cache").c_str()));
}

TEST_F(EpubSourceIdentityTest, BookLoadRejectsStaleSectionsWhenCssInvalidationFails) {
  const auto identity = identify(makeCssTestEpub());
  Epub epub(EPUB_PATH, "/.crosspoint");
  const std::string cachePath = epub.getCachePath();
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str()));
  ASSERT_TRUE(epub.bindCurrentSource());
  Storage.setFile(cachePath + "/book.bin", makeBookCache(identity));
  const std::string sectionsPath = cachePath + "/sections";
  ASSERT_TRUE(Storage.mkdir(sectionsPath.c_str()));
  Storage.setFile(sectionsPath + "/section_0.bin", {0xCC});
  Storage.failRemoveDirOnce();

  EXPECT_FALSE(epub.load(false, false));
  EXPECT_TRUE(Storage.exists((sectionsPath + "/section_0.bin").c_str()));
}

TEST_F(EpubSourceIdentityTest, StorePublishesRecoversAndProtectsNewerSibling) {
  const auto first = identify(makeZip());
  const auto second = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, first), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::Saved);

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, second);

  auto& primary = Storage.mutableFile(std::string(CACHE_PATH) + "/source_identity.bin");
  primary[SourceIdentityCodec::PAYLOAD_OFFSET] ^= 1U;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Backup);
  EXPECT_EQ(loaded, first);

  auto newer = Storage.file(std::string(CACHE_PATH) + "/source_identity.bin.bak");
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(std::string(CACHE_PATH) + "/source_identity.bin.bak", std::move(newer));
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::NewerVersion);
}

TEST_F(EpubSourceIdentityTest, StoreWriteFaultLeavesVerifiedFallback) {
  const auto first = identify(makeZip());
  const auto second = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, first), SourceIdentityStore::SaveStatus::Saved);
  Storage.failRenameOnce();
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, second), SourceIdentityStore::SaveStatus::IoError);

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, first);
}

TEST_F(EpubSourceIdentityTest, TrulyMissingSidecarCanBeAdoptedForLegacyMigration) {
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Missing);
  const auto current = identify(makeZip());
  EXPECT_EQ(SourceIdentityStore::save(CACHE_PATH, current), SourceIdentityStore::SaveStatus::Saved);
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, current);
}

TEST_F(EpubSourceIdentityTest, PreparedReplacementRetainsOldIdentityAndCancelsAfterReboot) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  ZipFile::SourceIdentity loaded;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_TRUE(SourceIdentityStore::isReplacementBarrier(loaded));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, PreparedReplacementKeepsBarrierForPublishedNewSource) {
  const auto oldIdentity = identify(makeZip());
  const auto newIdentity = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, newIdentity),
            SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished);
  ZipFile::SourceIdentity loaded;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_TRUE(SourceIdentityStore::isReplacementBarrier(loaded));
}

TEST_F(EpubSourceIdentityTest, LegacyStateGetsRecoverableOldIdentityBeforeBarrier) {
  ZipFile::SourceIdentity missing;
  ASSERT_EQ(SourceIdentityStore::load(CACHE_PATH, missing), SourceIdentityStore::LoadStatus::Missing);
  const auto currentIdentity = identify(makeZip());

  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &currentIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);
  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, currentIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
}

TEST_F(EpubSourceIdentityTest, BarrierPreparationFaultLeavesOldIdentityLoadable) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  Storage.failRenameOnce();

  EXPECT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::IoError);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, FailedNewPathPublicationCancelsBarrierWithoutInventingIdentity) {
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, nullptr),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);
  ASSERT_TRUE(SourceIdentityStore::cancelReplacement(CACHE_PATH));

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Missing);
}

TEST_F(EpubSourceIdentityTest, SyncFaultDuringPreparationDoesNotHideOldIdentity) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  Storage.failSyncOnce();

  EXPECT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::IoError);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, RecoveryNeverDeletesNewerTemporarySidecar) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  const std::string tempPath = std::string(CACHE_PATH) + "/source_identity.bin.tmp";
  auto newer = Storage.file(std::string(CACHE_PATH) + "/source_identity.bin.bak");
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(tempPath, newer);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::NewerVersion);
  EXPECT_FALSE(SourceIdentityStore::cancelReplacement(CACHE_PATH));
  EXPECT_EQ(Storage.file(tempPath), newer);
}

TEST_F(EpubSourceIdentityTest, RecoveryProtectsSiblingsWhenBarrierPrimaryIsMissing) {
  const auto oldIdentity = identify(makeZip());
  const auto newIdentity = identify(makeZip(0x87654321U));
  ASSERT_EQ(SourceIdentityStore::prepareReplacement(CACHE_PATH, &oldIdentity),
            SourceIdentityStore::PrepareReplacementStatus::Prepared);

  const std::string primaryPath = std::string(CACHE_PATH) + "/source_identity.bin";
  const std::string tempPath = primaryPath + ".tmp";
  ASSERT_TRUE(Storage.rename(primaryPath.c_str(), tempPath.c_str()));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, newIdentity),
            SourceIdentityStore::RecoverReplacementStatus::ReplacementPublished);
  EXPECT_TRUE(Storage.exists(tempPath.c_str()));

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::RestoredCurrentSource);
  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, oldIdentity);
}

TEST_F(EpubSourceIdentityTest, RecoveryProtectsNewerTempWhenPrimaryIsMissing) {
  const auto oldIdentity = identify(makeZip());
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, oldIdentity), SourceIdentityStore::SaveStatus::Saved);

  const std::string primaryPath = std::string(CACHE_PATH) + "/source_identity.bin";
  const std::string tempPath = primaryPath + ".tmp";
  ASSERT_TRUE(Storage.rename(primaryPath.c_str(), tempPath.c_str()));
  auto newer = Storage.file(tempPath);
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;
  Storage.setFile(tempPath, newer);

  EXPECT_EQ(SourceIdentityStore::recoverReplacement(CACHE_PATH, oldIdentity),
            SourceIdentityStore::RecoverReplacementStatus::NewerVersion);
  EXPECT_EQ(Storage.file(tempPath), newer);
}

}  // namespace
