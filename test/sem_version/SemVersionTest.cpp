#include <gtest/gtest.h>

#include "SemVersionUtils.h"

static SemVersion parseOrFail(const std::string& versionString) {
  SemVersion version;
  EXPECT_TRUE(parse(versionString, version));
  return version;
}

TEST(SemVersion, ParseableVersionsWithoutPrereleaseTag) {
  SemVersion version = parseOrFail("1.2.3");
  EXPECT_EQ(version.major, 1);
  EXPECT_EQ(version.minor, 2);
  EXPECT_EQ(version.patch, 3);
  EXPECT_FALSE(version.prerelease);
}

TEST(SemVersion, ParseableVersionsWithPrereleaseTag) {
  SemVersion version = parseOrFail("1.2.3-rc");
  EXPECT_EQ(version.major, 1);
  EXPECT_EQ(version.minor, 2);
  EXPECT_EQ(version.patch, 3);
  EXPECT_TRUE(version.prerelease);
}

TEST(SemVersion, UnparseableVersions) {
  std::string versionStrings[] = {"", "foo", "1", "1.2", "1.2.3.4"};

  for (const auto& versionString : versionStrings) {
    SemVersion ignored;
    EXPECT_FALSE(parse(versionString, ignored));
  }
}
