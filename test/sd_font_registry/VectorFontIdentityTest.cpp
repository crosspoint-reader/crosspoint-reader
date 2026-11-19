#include <HalStorage.h>
#include <VectorFontIdentity.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {
void put32(std::string& bytes, size_t at, uint32_t value) {
  for (int i = 3; i >= 0; --i) {
    bytes[at + i] = static_cast<char>(value);
    value >>= 8;
  }
}
std::string font(const std::string& name, bool collection = false, bool otf = false) {
  const size_t base = collection ? 16 : 0;
  std::string bytes(base + 28, '\0');
  if (collection) {
    put32(bytes, 0, 0x74746366);
    put32(bytes, 8, 1);
    put32(bytes, 12, base);
  }
  put32(bytes, base, otf ? 0x4F54544F : 0x00010000);
  bytes[base + 5] = 1;
  put32(bytes, base + 12, 0x6E616D65);
  put32(bytes, base + 20, bytes.size());
  put32(bytes, base + 24, name.size());
  return bytes + name;
}
uint32_t source(const std::string& bytes, const char* filename) {
  fake::add("/font", bytes);
  auto file = Storage.open("/font");
  EXPECT_TRUE(file.seek(1));
  auto hash = vectorFontSourceHash(file, filename);
  EXPECT_EQ(file.position(), 1u);
  return hash;
}
TEST(VectorFontIdentityTest, NameHashSurvivesRenameAcrossSupportedContainers) {
  fake::reset();
  auto hash = source(font("name table"), "Old.ttf");
  EXPECT_EQ(hash, source(font("name table"), "New.ttf"));
  EXPECT_EQ(hash, source(font("name table", true), "Collection.ttc"));
  EXPECT_EQ(hash, source(font("name table", false, true), "OpenType.otf"));
  EXPECT_NE(hash, source(font("changed table"), "Old.ttf"));
}
TEST(VectorFontIdentityTest, HashesAllChunksAndFallsBackOnTruncation) {
  fake::reset();
  std::string name(200, 'a');
  auto before = source(font(name), "File.ttf");
  name.back() = 'b';
  EXPECT_NE(before, source(font(name), "File.ttf"));
  auto truncated = font(name);
  truncated.pop_back();
  EXPECT_EQ(source(truncated, "File.ttf"), source("invalid", "File.ttf"));
  EXPECT_NE(source("invalid", "File.ttf"), source("invalid", "Other.ttf"));
}
TEST(VectorFontIdentityTest, IdentityIncludesRolesSizeUseAndPresence) {
  uint32_t hashes[4] = {11, 22, 33, 44};
  auto id = vectorFontIdentity(hashes, 15, 14);
  EXPECT_NE(id, 0);
  EXPECT_EQ(id, vectorFontIdentity(hashes, 15, 14));
  EXPECT_NE(id, vectorFontIdentity(hashes, 15, 16));
  EXPECT_NE(id, vectorFontIdentity(hashes, 15, 14, true));
  EXPECT_NE(id, vectorFontIdentity(hashes, 7, 14));
  std::swap(hashes[0], hashes[2]);
  EXPECT_NE(id, vectorFontIdentity(hashes, 15, 14));
}
TEST(VectorFontIdentityTest, MalformedAndMissingTablesUseFilenameFallback) {
  fake::reset();
  const auto fallback = source("invalid", "File.ttf");
  auto missing = font("name");
  put32(missing, 12, 0x68656164);
  EXPECT_EQ(source(missing, "File.ttf"), fallback);
  auto outside = font("name");
  put32(outside, 20, 0xFFFFFFFFu);
  EXPECT_EQ(source(outside, "File.ttf"), fallback);
  auto empty = font("");
  EXPECT_EQ(source(empty, "File.ttf"), fallback);
  auto collection = font("name", true);
  put32(collection, 8, 0);
  EXPECT_EQ(source(collection, "File.ttf"), fallback);
  fake::failRead = 3;
  EXPECT_EQ(source(font(std::string(200, 'a')), "File.ttf"), fallback);
}
TEST(VectorFontIdentityTest, NonNameChangesDoNotAffectIdentity) {
  fake::reset();
  auto bytes = font("name");
  const auto before = source(bytes, "File.ttf");
  bytes[16] = 42;  // Directory checksum, outside the name table.
  EXPECT_EQ(source(bytes, "File.ttf"), before);
}

TEST(VectorFontIdentityTest, ClosedFileFallsBackWhenPositionCannotBeRestored) {
  fake::reset();
  const auto fallback = source("invalid", "File.ttf");
  HalFile closed;
  EXPECT_EQ(vectorFontSourceHash(closed, "File.ttf"), fallback);
}
}  // namespace
