#include <gtest/gtest.h>

#include "XmlParserUtils.h"

TEST(XmlParserUtils, MatchesUnprefixedAndPrefixedLocalNames) {
  EXPECT_TRUE(xmlLocalNameEquals("package", "package"));
  EXPECT_TRUE(xmlLocalNameEquals("opf:package", "package"));
  EXPECT_TRUE(xmlLocalNameEquals("ns0:package", "package"));
  EXPECT_TRUE(xmlLocalNameEquals("metadata", "metadata"));
  EXPECT_TRUE(xmlLocalNameEquals("ns0:manifest", "manifest"));
  EXPECT_TRUE(xmlLocalNameEquals("ns0:spine", "spine"));
  EXPECT_TRUE(xmlLocalNameEquals("ns0:itemref", "itemref"));
  EXPECT_TRUE(xmlLocalNameEquals("dc:title", "title"));
}

TEST(XmlParserUtils, RejectsDifferentLocalNames) {
  EXPECT_FALSE(xmlLocalNameEquals("opf:metadata", "package"));
  EXPECT_FALSE(xmlLocalNameEquals("manifest", "spine"));
  EXPECT_FALSE(xmlLocalNameEquals("dc:creator", "title"));
}

TEST(XmlParserUtils, HandlesNullQName) {
  EXPECT_STREQ(xmlLocalName(nullptr), "");
  EXPECT_FALSE(xmlLocalNameEquals(nullptr, "package"));
}
