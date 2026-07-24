#include <HalStorage.h>
#include <Txt.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Epub/SourceIdentityCodec.h"
#include "Epub/SourceIdentityStore.h"

namespace {
constexpr char BOOK_PATH[] = "/book.txt";
constexpr char CACHE_PATH[] = "/.crosspoint/txt_123";

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

ZipFile::SourceIdentity loadIdentity(const std::string& content) {
  Storage.setFile(BOOK_PATH, bytes(content));
  Txt txt(BOOK_PATH, "/.crosspoint");
  EXPECT_TRUE(txt.load());
  ZipFile::SourceIdentity identity;
  EXPECT_TRUE(txt.getSourceIdentity(identity));
  return identity;
}
}  // namespace

class TxtSourceIdentityTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(TxtSourceIdentityTest, StreamsCompleteFileIntoRawCrcAndFnvIdentity) {
  Storage.setFile(BOOK_PATH, bytes("abc"));
  Txt txt(BOOK_PATH, "/.crosspoint");

  ASSERT_TRUE(txt.load());
  EXPECT_EQ(txt.getFileSize(), 3u);
  ZipFile::SourceIdentity identity;
  ASSERT_TRUE(txt.getSourceIdentity(identity));
  EXPECT_TRUE(identity.isRawFile());
  EXPECT_EQ(identity.fileSize, 3u);
  EXPECT_EQ(identity.rawFileCrc32(), 0x352441C2u);
  EXPECT_EQ(identity.rawFileFnv64(), 0xE71FA2190541574Bull);
  EXPECT_LE(Storage.maxRead(), 2048u);
}

TEST_F(TxtSourceIdentityTest, EmptyFileHasAValidDistinctRawIdentity) {
  Storage.setFile(BOOK_PATH, {});
  Txt txt(BOOK_PATH, "/.crosspoint");

  ASSERT_TRUE(txt.load());
  ZipFile::SourceIdentity identity;
  ASSERT_TRUE(txt.getSourceIdentity(identity));
  EXPECT_TRUE(identity.isRawFile());
  EXPECT_EQ(identity.fileSize, 0u);
  EXPECT_EQ(identity.rawFileCrc32(), 0u);
  EXPECT_EQ(identity.rawFileFnv64(), 14695981039346656037ULL);
}

TEST_F(TxtSourceIdentityTest, RemovesTxtAndMarkdownExtensionsFromDisplayTitle) {
  EXPECT_EQ(Txt("/books/novel.txt", "/.crosspoint").getTitle(), "novel");
  EXPECT_EQ(Txt("/books/notes.md", "/.crosspoint").getTitle(), "notes");
}

TEST_F(TxtSourceIdentityTest, SameSizeReplacementChangesIdentity) {
  const ZipFile::SourceIdentity first = loadIdentity("abc");
  const ZipFile::SourceIdentity second = loadIdentity("abd");
  EXPECT_NE(first, second);
  EXPECT_EQ(first.fileSize, second.fileSize);
}

TEST_F(TxtSourceIdentityTest, RejectsFileThatChangesSizeDuringFingerprinting) {
  Storage.setFile(BOOK_PATH, std::vector<uint8_t>(3000, 0x5A));
  Storage.growOnReadCall(1);
  Txt txt(BOOK_PATH, "/.crosspoint");

  EXPECT_FALSE(txt.load());
  ZipFile::SourceIdentity identity;
  EXPECT_FALSE(txt.getSourceIdentity(identity));
}

TEST_F(TxtSourceIdentityTest, RejectsFilesLargerThanUint32BeforeReadingPayload) {
  Storage.setFile(BOOK_PATH, {0x5A});
  Storage.reportFileSize(BOOK_PATH, static_cast<uint64_t>(UINT32_MAX) + 1U);
  Txt txt(BOOK_PATH, "/.crosspoint");

  EXPECT_FALSE(txt.load());
  EXPECT_EQ(Storage.maxRead(), 0u);
}

TEST_F(TxtSourceIdentityTest, RejectsReadAndCloseFailuresWithoutPublishingIdentity) {
  Storage.setFile(BOOK_PATH, bytes("content"));
  Storage.makeUnreadable(BOOK_PATH);
  Txt unreadable(BOOK_PATH, "/.crosspoint");
  EXPECT_FALSE(unreadable.load());

  Storage.makeReadable(BOOK_PATH);
  Storage.failCloseFor(BOOK_PATH);
  Txt closeFailure(BOOK_PATH, "/.crosspoint");
  EXPECT_FALSE(closeFailure.load());
}

TEST_F(TxtSourceIdentityTest, CodecRoundTripsRawIdentityWithoutChangingEpubEnvelope) {
  const ZipFile::SourceIdentity raw = ZipFile::SourceIdentity::forRawFile(1234, 0x89ABCDEF, 0x0123456789ABCDEFULL);
  SourceIdentityCodec::Encoded encoded;
  ASSERT_TRUE(SourceIdentityCodec::encode(raw, encoded));
  EXPECT_EQ(encoded[SourceIdentityCodec::VERSION_OFFSET], SourceIdentityCodec::VERSION);
  EXPECT_EQ(SourceIdentityCodec::readU16(encoded.data() + SourceIdentityCodec::PAYLOAD_LENGTH_OFFSET),
            SourceIdentityCodec::PAYLOAD_SIZE);

  ZipFile::SourceIdentity decoded;
  EXPECT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::OK);
  EXPECT_EQ(decoded, raw);
  EXPECT_TRUE(decoded.isRawFile());

  const ZipFile::SourceIdentity epub{100, 20, 40, 2, 0xAABBCCDDEEFF0011ULL};
  ASSERT_TRUE(SourceIdentityCodec::encode(epub, encoded));
  EXPECT_EQ(SourceIdentityCodec::decode(encoded.data(), encoded.size(), decoded),
            SourceIdentityCodec::DecodeStatus::OK);
  EXPECT_EQ(decoded, epub);
  EXPECT_FALSE(decoded.isRawFile());
}

TEST_F(TxtSourceIdentityTest, StorePublishesAndRecoversRawIdentity) {
  const ZipFile::SourceIdentity raw = loadIdentity("stored raw text");
  ASSERT_EQ(SourceIdentityStore::save(CACHE_PATH, raw), SourceIdentityStore::SaveStatus::Saved);

  ZipFile::SourceIdentity loaded;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Primary);
  EXPECT_EQ(loaded, raw);

  auto& primary = Storage.mutableFile(std::string(CACHE_PATH) + "/source_identity.bin");
  primary[SourceIdentityCodec::PAYLOAD_OFFSET] ^= 1U;
  EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::Invalid);
}

TEST_F(TxtSourceIdentityTest, StoreLoadFailsClosedForNewerIdentityInAnySibling) {
  const ZipFile::SourceIdentity raw = loadIdentity("stored raw text");
  SourceIdentityCodec::Encoded valid;
  ASSERT_TRUE(SourceIdentityCodec::encode(raw, valid));
  SourceIdentityCodec::Encoded newer = valid;
  newer[SourceIdentityCodec::VERSION_OFFSET] = SourceIdentityCodec::VERSION + 1;

  const std::array<std::string, 3> paths = {std::string(CACHE_PATH) + "/source_identity.bin",
                                            std::string(CACHE_PATH) + "/source_identity.bin.bak",
                                            std::string(CACHE_PATH) + "/source_identity.bin.tmp"};
  for (size_t index = 0; index < paths.size(); ++index) {
    SCOPED_TRACE(paths[index]);
    Storage.reset();
    Storage.setFile(index == 0 ? paths[1] : paths[0], std::vector<uint8_t>(valid.begin(), valid.end()));
    Storage.setFile(paths[index], std::vector<uint8_t>(newer.begin(), newer.end()));

    ZipFile::SourceIdentity loaded;
    EXPECT_EQ(SourceIdentityStore::load(CACHE_PATH, loaded), SourceIdentityStore::LoadStatus::NewerVersion);
  }
}
