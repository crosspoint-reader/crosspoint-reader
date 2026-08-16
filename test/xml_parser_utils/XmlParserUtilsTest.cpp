#include <gtest/gtest.h>

#include "XmlParserUtils.h"

TEST(XmlParserUtils, MatchesLocalNamesRegardlessOfPrefix) {
  EXPECT_TRUE(xmlLocalNameEquals("package", "package"));
  EXPECT_TRUE(xmlLocalNameEquals("ns0:package", "package"));
  EXPECT_FALSE(xmlLocalNameEquals("ns0:metadata", "package"));
}
