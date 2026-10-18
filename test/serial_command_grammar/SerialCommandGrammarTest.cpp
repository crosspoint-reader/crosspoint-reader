// The argument grammar of the serial test commands (docs/serial-test-api.md): what a host may send
// after CMD:BTN, CMD:ACT:READER, CMD:SET, CMD:TOUCH/SWIPE and CMD:LOG, and how CMD:STATE writes its
// key=value pairs so a host can split them again.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/SerialCommandGrammar.h"

using SerialCommandGrammar::PressKind;
using SerialCommandGrammar::ReaderTarget;

namespace {

// Splits a STATE line the way the doc tells a host to: on spaces outside double quotes, with
// backslash escapes inside them. The inverse of appendField().
std::vector<std::pair<std::string, std::string>> splitFields(const std::string& line) {
  std::vector<std::pair<std::string, std::string>> fields;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) break;
    const size_t eq = line.find('=', i);
    std::string key = line.substr(i, eq - i);
    i = eq + 1;
    std::string value;
    if (i < line.size() && line[i] == '"') {
      ++i;
      while (i < line.size() && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < line.size()) ++i;
        value += line[i++];
      }
      ++i;  // closing quote
    } else {
      while (i < line.size() && line[i] != ' ') value += line[i++];
    }
    fields.emplace_back(key, value);
  }
  return fields;
}

}  // namespace

TEST(SerialCommandGrammar, ButtonNamesAndModifiers) {
  std::string_view name;
  PressKind kind = PressKind::Long;
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("BACK", name, kind));
  EXPECT_EQ(name, "BACK");
  EXPECT_EQ(kind, PressKind::Short);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("CONFIRM:LONG", name, kind));
  EXPECT_EQ(name, "CONFIRM");
  EXPECT_EQ(kind, PressKind::Long);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("POWER:DOUBLE", name, kind));
  EXPECT_EQ(name, "POWER");
  EXPECT_EQ(kind, PressKind::Double);
}

TEST(SerialCommandGrammar, ATypoInTheModifierIsAnErrorNotAShortPress) {
  std::string_view name;
  PressKind kind = PressKind::Short;
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:LONGG", name, kind));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:", name, kind));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("", name, kind));
}

TEST(SerialCommandGrammar, ReaderTargetWithoutAPositionOpensAtTheSavedOne) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/The Anarchy - Dalrymple, William.epub", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Saved);
  EXPECT_EQ(target.path, "/test/The Anarchy - Dalrymple, William.epub");
}

TEST(SerialCommandGrammar, ReaderTargetSpineAndPage) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@3:12", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::SpinePage);
  EXPECT_EQ(target.path, "/test/a.epub");
  EXPECT_EQ(target.spine, 3);
  EXPECT_EQ(target.page, 12);
}

TEST(SerialCommandGrammar, ReaderTargetPercent) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@50%", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Percent);
  EXPECT_EQ(target.path, "/test/a.epub");
  EXPECT_EQ(target.percent, 50);
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@100%", target));
  EXPECT_EQ(target.percent, 100);
}

// An '@' in a file name is not a position unless all of what follows the last one is one.
TEST(SerialCommandGrammar, AnAtSignInTheFileNameStaysInThePath) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/books/me@home.epub", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Saved);
  EXPECT_EQ(target.path, "/books/me@home.epub");
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/books/me@home.epub@2:1", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::SpinePage);
  EXPECT_EQ(target.path, "/books/me@home.epub");
}

TEST(SerialCommandGrammar, ReaderPositionsOutOfRangeAreRefused) {
  ReaderTarget target;
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@0:1", target)) << "spines count from 1";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@1:0", target)) << "pages count from 1";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@101%", target));
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("@3:4", target)) << "no path";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("", target));
}

TEST(SerialCommandGrammar, KeyValueSplitsAtTheFirstEquals) {
  std::string_view key;
  std::string_view value;
  ASSERT_TRUE(SerialCommandGrammar::splitKeyValue("opdsUrl=http://x/?a=b", key, value));
  EXPECT_EQ(key, "opdsUrl");
  EXPECT_EQ(value, "http://x/?a=b");
  ASSERT_TRUE(SerialCommandGrammar::splitKeyValue("name=", key, value));
  EXPECT_EQ(value, "");
  EXPECT_FALSE(SerialCommandGrammar::splitKeyValue("=1", key, value));
  EXPECT_FALSE(SerialCommandGrammar::splitKeyValue("fontSize", key, value));
}

TEST(SerialCommandGrammar, IntegersAndLists) {
  int value = 0;
  EXPECT_TRUE(SerialCommandGrammar::parseInt("42", value));
  EXPECT_EQ(value, 42);
  EXPECT_TRUE(SerialCommandGrammar::parseInt("-7", value));
  EXPECT_EQ(value, -7);
  EXPECT_FALSE(SerialCommandGrammar::parseInt("", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("-", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("12a", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("99999999999", value));
  // The limits, and numbers whose last step overflows 32 bits: on the device `long` was 32 bits
  // and they wrapped back into range before the range check (the host's 64-bit long hid that).
  EXPECT_TRUE(SerialCommandGrammar::parseInt("2147483647", value));
  EXPECT_EQ(value, 2147483647);
  EXPECT_TRUE(SerialCommandGrammar::parseInt("-2147483647", value));
  EXPECT_EQ(value, -2147483647);
  EXPECT_FALSE(SerialCommandGrammar::parseInt("2147483648", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("9999999999", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("4294967296", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("999999999999", value));

  int xy[2] = {};
  ASSERT_TRUE(SerialCommandGrammar::parseIntList("120,640", xy, 2));
  EXPECT_EQ(xy[0], 120);
  EXPECT_EQ(xy[1], 640);
  int swipe[4] = {};
  ASSERT_TRUE(SerialCommandGrammar::parseIntList("10,700,10,100", swipe, 4));
  EXPECT_EQ(swipe[3], 100);
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1,2,3", xy, 2)) << "too many";
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1", xy, 2)) << "too few";
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1,", xy, 2));
}

TEST(SerialCommandGrammar, LogLevelsByTagOrName) {
  uint8_t level = 9;
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("ERR", level));
  EXPECT_EQ(level, 0);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("info", level));
  EXPECT_EQ(level, 1);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("DEBUG", level));
  EXPECT_EQ(level, 2);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("trc", level));
  EXPECT_EQ(level, 3);
  EXPECT_FALSE(SerialCommandGrammar::parseLogLevel("WARN", level)) << "the firmware has no WARN level";
}

TEST(SerialCommandGrammar, PlainValuesAreWrittenBare) {
  std::string line = "STATE:activity=Home";
  SerialCommandGrammar::appendField(line, "sel", 3L);
  SerialCommandGrammar::appendField(line, "page", "4/12");
  EXPECT_EQ(line, "STATE:activity=Home sel=3 page=4/12");
}

TEST(SerialCommandGrammar, ValuesWithSpacesOrQuotesAreQuotedAndRoundTrip) {
  std::string line;
  SerialCommandGrammar::appendField(line, "book", "/test/The Anarchy - Dalrymple, William.epub");
  SerialCommandGrammar::appendField(line, "label", R"(Say "hi" \ bye)");
  SerialCommandGrammar::appendField(line, "popup", "Nicht\ngefunden");
  SerialCommandGrammar::appendField(line, "stack", "");
  SerialCommandGrammar::appendField(line, "eq", "a=b");
  EXPECT_EQ(line.find('\n'), std::string::npos) << "a reply is one line";
  const auto fields = splitFields(line);
  ASSERT_EQ(fields.size(), 5u);
  EXPECT_EQ(fields[0].second, "/test/The Anarchy - Dalrymple, William.epub");
  EXPECT_EQ(fields[1].second, R"(Say "hi" \ bye)");
  EXPECT_EQ(fields[2].second, "Nicht gefunden");
  EXPECT_EQ(fields[3].first, "stack");
  EXPECT_EQ(fields[3].second, "");
  EXPECT_EQ(fields[4].second, "a=b");
}

TEST(SerialCommandGrammar, NonAsciiTextIsLeftAlone) {
  std::string line;
  SerialCommandGrammar::appendField(line, "label", "Übertragung");
  EXPECT_EQ(line, " label=Übertragung");
}
