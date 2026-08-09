#include <gtest/gtest.h>

#include "VersionUtils.h"

static Version parseOrFail(const std::string& versionString) {
	Version version;
	EXPECT_TRUE(parse(versionString, version));
	return version;
}

TEST(Version, ParseableVersionsWithoutPrereleaseTag) {
	std::string versionStrings[] = {
		"1.2.3",
		"v1.2.3"
	};
	
	for (const auto& versionString : versionStrings) {
		Version version = parseOrFail(versionString);
		EXPECT_EQ(version.major, 1);
		EXPECT_EQ(version.minor, 2);
		EXPECT_EQ(version.patch, 3);
		EXPECT_FALSE(version.prerelease);
	}
}

TEST(Version, ParseableVersionsWithPrereleaseTag) {
	std::string versionStrings[] = {
		"1.2.3-rc",
		"v1.2.3-rc-1"
	};
	
	for (const auto& versionString : versionStrings) {
		Version version = parseOrFail(versionString);
		EXPECT_EQ(version.major, 1);
		EXPECT_EQ(version.minor, 2);
		EXPECT_EQ(version.patch, 3);
		EXPECT_TRUE(version.prerelease);
	}
}

TEST(Version, UnparseableVersions) {
	Version ignored;
	EXPECT_FALSE(parse("foo", ignored));
	EXPECT_FALSE(parse("1", ignored));
	EXPECT_FALSE(parse("1.2", ignored));
	EXPECT_FALSE(parse("1.2.3.4", ignored));
}

