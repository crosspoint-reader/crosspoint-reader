#include <HttpHeader.h>
#include <gtest/gtest.h>

TEST(HttpHeaderTest, ParsesTrimmedNameAndValue) {
  HttpHeader header;
  parseHttpHeaderLine("  CF-Access-Client-Id :\t abc123  ", header);
  EXPECT_EQ(header.name, "CF-Access-Client-Id");
  EXPECT_EQ(header.value, "abc123");
}

TEST(HttpHeaderTest, SplitsOnFirstColonOnly) {
  HttpHeader header;
  parseHttpHeaderLine("X-Target: http://host:8080", header);
  EXPECT_EQ(header.name, "X-Target");
  EXPECT_EQ(header.value, "http://host:8080");
}

TEST(HttpHeaderTest, MissingColonIsBareNameAndClearsValue) {
  HttpHeader header{"Old", "stale"};
  parseHttpHeaderLine(" X-Flag ", header);
  EXPECT_EQ(header.name, "X-Flag");
  EXPECT_EQ(header.value, "");
}

TEST(HttpHeaderTest, BlankLineClearsHeader) {
  HttpHeader header{"Old", "stale"};
  parseHttpHeaderLine("   ", header);
  EXPECT_EQ(header.name, "");
  EXPECT_EQ(header.value, "");
}

TEST(HttpHeaderTest, FormatsAndMasks) {
  EXPECT_EQ(formatHttpHeaderLine({"", "orphan"}), "");
  EXPECT_EQ(formatHttpHeaderLine({"X-Flag", ""}), "X-Flag");
  EXPECT_EQ(formatHttpHeaderLine({"X-Key", "secret"}), "X-Key: secret");
  EXPECT_EQ(formatHttpHeaderLine({"X-Key", "secret"}, true), "X-Key: ******");
  EXPECT_EQ(formatHttpHeaderLine({"X-Flag", ""}, true), "X-Flag");
}

TEST(HttpHeaderTest, FormatRoundTripsThroughParse) {
  const HttpHeader original{"Authorization", "Bearer a:b"};
  HttpHeader parsed;
  parseHttpHeaderLine(formatHttpHeaderLine(original), parsed);
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.value, original.value);
}
