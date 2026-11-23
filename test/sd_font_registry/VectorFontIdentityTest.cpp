#include <VectorFontIdentity.h>
#include <gtest/gtest.h>

namespace {
TEST(VectorFontIdentityTest, IdentityIsStableAndNonzero) {
  const auto id = vectorFontIdentity("Family", 0x01020304u, 14);
  EXPECT_NE(id, 0);
  EXPECT_EQ(id, vectorFontIdentity("Family", 0x01020304u, 14));
  EXPECT_EQ(id, vectorFontIdentity("Family", 0x01020304u, 14, false));
}

TEST(VectorFontIdentityTest, FamilyNamesSeparateIdenticalContent) {
  EXPECT_NE(vectorFontIdentity("Family", 123, 14), vectorFontIdentity("Other", 123, 14));
}

TEST(VectorFontIdentityTest, EveryContentKeyByteAffectsIdentity) {
  const uint32_t contentKey = 0x01020304u;
  const auto id = vectorFontIdentity("Family", contentKey, 14);
  for (int shift = 0; shift < 32; shift += 8) {
    SCOPED_TRACE(shift);
    EXPECT_NE(id, vectorFontIdentity("Family", contentKey ^ (1u << shift), 14));
  }
}

TEST(VectorFontIdentityTest, PointSizesHaveSeparateIdentities) {
  EXPECT_NE(vectorFontIdentity("Family", 123, 14), vectorFontIdentity("Family", 123, 16));
}

TEST(VectorFontIdentityTest, ReaderAndUiHaveSeparateIdentitiesAtTheSameSize) {
  EXPECT_NE(vectorFontIdentity("Family", 123, 12), vectorFontIdentity("Family", 123, 12, true));
}

TEST(VectorFontIdentityTest, EmptyFamilyAndZeroContentKeyRemainDeterministicAndNonzero) {
  const auto id = vectorFontIdentity("", 0, 14);
  EXPECT_NE(id, 0);
  EXPECT_EQ(id, vectorFontIdentity("", 0, 14));
  EXPECT_EQ(id, vectorFontIdentity(nullptr, 0, 14));
  EXPECT_NE(id, vectorFontIdentity("", 1, 14));
}
}  // namespace
