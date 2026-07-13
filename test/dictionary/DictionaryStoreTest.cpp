#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "lib/Dictionary/DictionaryStore.h"

#ifndef DICTIONARY_RESOURCES_DIR
#error "DICTIONARY_RESOURCES_DIR must be defined by the build system"
#endif

namespace {

// File-backed DictByteSource for host tests.
struct FileSource {
  explicit FileSource(const std::string& path) : file(fopen(path.c_str(), "rb")) {}
  ~FileSource() {
    if (file != nullptr) {
      fclose(file);
    }
  }

  DictByteSource source() {
    DictByteSource s;
    s.ctx = file;
    s.readAt = [](void* ctx, uint32_t off, void* buf, uint32_t len) {
      auto* f = static_cast<FILE*>(ctx);
      if (f == nullptr || fseek(f, static_cast<long>(off), SEEK_SET) != 0) {
        return false;
      }
      return fread(buf, 1, len, f) == len;
    };
    return s;
  }

  FILE* file;
};

// In-memory DictByteSource for corruption tests.
struct MemSource {
  explicit MemSource(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}

  DictByteSource source() {
    DictByteSource s;
    s.ctx = this;
    s.readAt = [](void* ctx, uint32_t off, void* buf, uint32_t len) {
      auto* self = static_cast<MemSource*>(ctx);
      if (static_cast<uint64_t>(off) + len > self->data.size()) {
        return false;
      }
      memcpy(buf, self->data.data() + off, len);
      return true;
    };
    return s;
  }

  std::vector<uint8_t> data;
};

std::string fixturePath() { return std::string(DICTIONARY_RESOURCES_DIR) + "/mini.cpd"; }

std::vector<uint8_t> readFixtureBytes() {
  FILE* f = fopen(fixturePath().c_str(), "rb");
  EXPECT_NE(f, nullptr);
  std::vector<uint8_t> bytes;
  uint8_t buf[512];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    bytes.insert(bytes.end(), buf, buf + n);
  }
  fclose(f);
  return bytes;
}

class DictionaryStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fileSource = std::make_unique<FileSource>(fixturePath());
    ASSERT_NE(fileSource->file, nullptr) << "fixture missing: " << fixturePath();
    ASSERT_TRUE(store.open(fileSource->source()));
  }

  bool lookup(const char* word) { return store.lookup(word, definition, matchedKey); }

  std::unique_ptr<FileSource> fileSource;
  DictionaryStore store;
  std::string definition;
  char matchedKey[DictionaryStore::KEY_LEN + 1] = {};
};

TEST_F(DictionaryStoreTest, OpenReadsHeader) {
  EXPECT_TRUE(store.isOpen());
  EXPECT_STREQ(store.title(), "Mini Test Dictionary");
}

TEST_F(DictionaryStoreTest, RejectsBadMagic) {
  auto bytes = readFixtureBytes();
  bytes[0] = 'X';
  MemSource corrupt(bytes);
  DictionaryStore s;
  EXPECT_FALSE(s.open(corrupt.source()));
}

TEST_F(DictionaryStoreTest, RejectsInconsistentGeometry) {
  auto bytes = readFixtureBytes();
  bytes[4] += 1;  // entryCount no longer matches entriesOffset
  MemSource corrupt(bytes);
  DictionaryStore s;
  EXPECT_FALSE(s.open(corrupt.source()));
}

TEST_F(DictionaryStoreTest, ExactLookup) {
  ASSERT_TRUE(lookup("kalem"));
  EXPECT_STREQ(matchedKey, "kalem");
  EXPECT_EQ(definition, "pen");
}

TEST_F(DictionaryStoreTest, LookupIsCaseInsensitive) {
  ASSERT_TRUE(lookup("KALEM"));
  EXPECT_STREQ(matchedKey, "kalem");
}

TEST_F(DictionaryStoreTest, TurkishDottedCapitalINormalizes) {
  // Fixture was built with --turkish-keys: İstanbul was indexed as "istanbul".
  ASSERT_TRUE(lookup("İstanbul"));
  EXPECT_STREQ(matchedKey, "istanbul");
  EXPECT_EQ(definition, "city on the Bosphorus");
}

TEST_F(DictionaryStoreTest, NonAsciiHeadword) {
  ASSERT_TRUE(lookup("Şeker"));
  EXPECT_STREQ(matchedKey, "şeker");
}

TEST_F(DictionaryStoreTest, DuplicateHeadwordsAreMerged) {
  ASSERT_TRUE(lookup("kitap"));
  EXPECT_NE(definition.find("book"), std::string::npos);
  EXPECT_NE(definition.find("volume; tome"), std::string::npos);
}

TEST_F(DictionaryStoreTest, PrefixFallbackFindsLongestStem) {
  // No exact entry; must fall back to "kitap", not give up.
  ASSERT_TRUE(lookup("kitaplarımızdan"));
  EXPECT_STREQ(matchedKey, "kitap");
}

TEST_F(DictionaryStoreTest, PrefixFallbackPicksLongestCandidate) {
  // "gitti" misses; both "git" and "gitmek" exist but only "git" is a prefix.
  ASSERT_TRUE(lookup("gitti"));
  EXPECT_STREQ(matchedKey, "git");
}

TEST_F(DictionaryStoreTest, FirstAndLastEntries) {
  ASSERT_TRUE(lookup("aaa"));
  EXPECT_EQ(definition, "first entry");
  ASSERT_TRUE(lookup("zzz"));
  EXPECT_EQ(definition, "last entry");
}

TEST_F(DictionaryStoreTest, MissesReturnFalse) {
  EXPECT_FALSE(lookup("yok"));
  EXPECT_FALSE(lookup(""));
  // Shorter than MIN_PREFIX_KEY_LEN stems must not match via prefix fallback.
  EXPECT_FALSE(lookup("evler"));
}

}  // namespace
