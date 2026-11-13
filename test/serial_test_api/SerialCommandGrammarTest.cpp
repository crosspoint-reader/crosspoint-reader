#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "util/SerialCommandGrammar.h"

using SerialCommandGrammar::appendClippedTail;
using SerialCommandGrammar::parseButtonArg;
using SerialCommandGrammar::parseInt;
using SerialCommandGrammar::PressKind;
using SerialCommandGrammar::ReplyLine;
using SerialCommandGrammar::TailField;

namespace {

constexpr int MAX_HOLD = 10000;

struct Press {
  bool ok = false;
  std::string name;
  PressKind kind = PressKind::Short;
  int holdMs = 0;
};

Press press(const std::string_view arg) {
  Press p;
  std::string_view name;
  p.ok = parseButtonArg(arg, name, p.kind, p.holdMs, MAX_HOLD);
  p.name = std::string(name);
  return p;
}

}  // namespace

TEST(SerialCommandGrammar, TrimDropsSpacesTabsAndCarriageReturn) {
  EXPECT_EQ(SerialCommandGrammar::trim("  STATE\t\r"), "STATE");
  EXPECT_EQ(SerialCommandGrammar::trim(""), "");
}

TEST(SerialCommandGrammar, SplitHeadCutsAtTheFirstColon) {
  std::string_view head;
  std::string_view rest;
  SerialCommandGrammar::splitHead("ACT:READER:/books/a:b.epub", head, rest);
  EXPECT_EQ(head, "ACT");
  EXPECT_EQ(rest, "READER:/books/a:b.epub");
  SerialCommandGrammar::splitHead("STATE", head, rest);
  EXPECT_EQ(head, "STATE");
  EXPECT_TRUE(rest.empty());
}

TEST(SerialCommandGrammar, ParseIntTakesSignedDecimalsInRange) {
  int v = 0;
  EXPECT_TRUE(parseInt("0", v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(parseInt("-42", v));
  EXPECT_EQ(v, -42);
  EXPECT_TRUE(parseInt("+7", v));
  EXPECT_EQ(v, 7);
  EXPECT_TRUE(parseInt("2147483647", v));
  EXPECT_EQ(v, 2147483647);
  EXPECT_TRUE(parseInt("-2147483648", v));
  EXPECT_EQ(v, std::numeric_limits<int>::min());
}

TEST(SerialCommandGrammar, ParseIntRefusesJunkAndOverflow) {
  int v = 5;
  EXPECT_FALSE(parseInt("", v));
  EXPECT_FALSE(parseInt("-", v));
  EXPECT_FALSE(parseInt("12a", v));
  EXPECT_FALSE(parseInt(" 12", v));
  EXPECT_FALSE(parseInt("2147483648", v));
  EXPECT_FALSE(parseInt("-2147483649", v));
  // Would wrap back into range if the overflow were checked after the step.
  EXPECT_FALSE(parseInt("9999999999", v));
  EXPECT_EQ(v, 5);
}

TEST(SerialCommandGrammar, ButtonPressKinds) {
  Press p = press("CONFIRM");
  EXPECT_TRUE(p.ok);
  EXPECT_EQ(p.name, "CONFIRM");
  EXPECT_EQ(p.kind, PressKind::Short);

  p = press("BACK:LONG");
  EXPECT_TRUE(p.ok);
  EXPECT_EQ(p.name, "BACK");
  EXPECT_EQ(p.kind, PressKind::Long);

  p = press("POWER:DOUBLE");
  EXPECT_TRUE(p.ok);
  EXPECT_EQ(p.kind, PressKind::Double);

  p = press("DOWN:1500");
  EXPECT_TRUE(p.ok);
  EXPECT_EQ(p.kind, PressKind::Hold);
  EXPECT_EQ(p.holdMs, 1500);
}

TEST(SerialCommandGrammar, ButtonTyposAreErrorsNotShortPresses) {
  EXPECT_FALSE(press("").ok);
  EXPECT_FALSE(press(":LONG").ok);
  EXPECT_FALSE(press("BACK:LONGG").ok);
  EXPECT_FALSE(press("BACK:").ok);
  EXPECT_FALSE(press("BACK:0").ok);
  EXPECT_FALSE(press("BACK:-5").ok);
  EXPECT_FALSE(press("BACK:+5").ok);
  EXPECT_FALSE(press("BACK:10001").ok);
  EXPECT_FALSE(press("BACK:LONG:LONG").ok);
}

TEST(SerialCommandGrammar, ReplyFieldsQuoteWhatWouldBreakSplitting) {
  char buffer[128];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE:");
  line.appendField("activity", "Home");
  line.appendField("stack", "");
  line.appendField("title", "A \"quoted\" title");
  line.appendField("path", "a\\b=c");
  line.appendField("page", 12L);
  line.appendField("delta", -3L);
  EXPECT_EQ(line.view(),
            "STATE: activity=Home stack=\"\" title=\"A \\\"quoted\\\" title\" path=\"a\\\\b=c\" page=12 delta=-3");
  EXPECT_FALSE(line.truncated());
}

// A range check of the unsigned field (STATE's uptime). The host's long is 64 bits, so the signed
// overload would print this value too: the device's 32-bit long is not reproduced here.
TEST(SerialCommandGrammar, AnUnsignedFieldKeepsItsFullRange) {
  char buffer[32];
  ReplyLine line(buffer, sizeof(buffer));
  line.appendField("uptime", 4294967295UL);
  EXPECT_EQ(line.view(), " uptime=4294967295");
}

TEST(SerialCommandGrammar, ReplyFieldsKeepTheReplyOnOneLine) {
  char buffer[64];
  ReplyLine line(buffer, sizeof(buffer));
  line.appendField("popup", "Line one\nLine two");
  EXPECT_EQ(line.view(), " popup=\"Line one Line two\"");
}

TEST(SerialCommandGrammar, ReplyLineStopsAtItsCapacity) {
  char buffer[8];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("HELP:");
  EXPECT_FALSE(line.truncated());
  line.append(" STATE");
  EXPECT_TRUE(line.truncated());
  EXPECT_EQ(line.view(), "HELP: ST");
}

TEST(SerialCommandGrammar, AClippedFieldThatFitsIsWrittenWhole) {
  char buffer[32];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("S:");
  EXPECT_TRUE(line.appendFieldClipped("t", "a b", 0));
  EXPECT_EQ(line.view(), "S: t=\"a b\"");
}

// 5 bytes left for the value: the 2-byte character would end at 6, so the cut comes before it.
TEST(SerialCommandGrammar, AClippedFieldIsCutBeforeAUtf8Character) {
  char buffer[10];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("S:");
  EXPECT_FALSE(line.appendFieldClipped("t", "x \xC3\xA9y", 0));
  EXPECT_EQ(line.view(), "S: t=\"x \"");
  EXPECT_FALSE(line.truncated());
}

// An escaped character is kept whole or left out: no lone backslash before the closing quote.
TEST(SerialCommandGrammar, AClippedFieldKeepsEscapesWhole) {
  char buffer[11];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("S:");
  EXPECT_FALSE(line.appendFieldClipped("t", "a b\"c", 0));
  EXPECT_EQ(line.view(), "S: t=\"a b\"");
}

TEST(SerialCommandGrammar, AClippedFieldLeavesTheReserveFree) {
  char buffer[24];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("S:");
  EXPECT_FALSE(line.appendFieldClipped("t", "abcdefghijklmnop", 10));
  line.append(" clipped=t");
  EXPECT_FALSE(line.truncated());
  EXPECT_EQ(line.view(), "S: t=abcdefg clipped=t");
}

TEST(SerialCommandGrammar, AClippedFieldWithNoRoomLeftTruncatesTheLine) {
  char buffer[6];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("S:");
  line.appendFieldClipped("title", "abc", 0);
  EXPECT_TRUE(line.truncated());
}

TEST(SerialCommandGrammar, AClippedTailThatFitsHasNoMarker) {
  char buffer[64];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE: page=4");
  TailField tail[] = {{"stack", "Home"}, {"title", "A b"}};
  EXPECT_TRUE(appendClippedTail(line, tail, 2));
  EXPECT_EQ(line.view(), "STATE: page=4 stack=Home title=\"A b\"");
}

// The title is cut so that `clipped=stack,title` would still fit after it.
TEST(SerialCommandGrammar, AClippedTailNamesTheFieldItCut) {
  char buffer[64];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE: page=4");
  TailField tail[] = {{"stack", "Home"}, {"title", "A long title here"}};
  EXPECT_TRUE(appendClippedTail(line, tail, 2));
  EXPECT_EQ(line.view(), "STATE: page=4 stack=Home title=\"A long titl\" clipped=title");
}

// The stack keeps room for the title's empty field: both are cut, both named, the line full.
TEST(SerialCommandGrammar, AClippedTailCanCutEveryField) {
  char buffer[56];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE: page=4");
  TailField tail[] = {{"stack", "Home,FileBrowser"}, {"title", "A long title here"}};
  EXPECT_TRUE(appendClippedTail(line, tail, 2));
  EXPECT_EQ(line.view(), "STATE: page=4 stack=Home, title=\"A \" clipped=stack,title");
}

TEST(SerialCommandGrammar, AClippedTailNamesAFieldCutBefore) {
  char buffer[64];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE: page=4");
  TailField tail[] = {{"stack", "Home", true}};
  EXPECT_TRUE(appendClippedTail(line, tail, 1));
  EXPECT_EQ(line.view(), "STATE: page=4 stack=Home clipped=stack");
}

TEST(SerialCommandGrammar, AClippedTailWithoutRoomIsRefused) {
  char buffer[30];
  ReplyLine line(buffer, sizeof(buffer));
  line.append("STATE: page=4");
  TailField tail[] = {{"stack", "Home"}, {"title", "A b"}};
  EXPECT_FALSE(appendClippedTail(line, tail, 2));
}
