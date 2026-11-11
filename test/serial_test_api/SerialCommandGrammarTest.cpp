#include <gtest/gtest.h>

#include <string>

#include "util/SerialCommandGrammar.h"

using SerialCommandGrammar::parseButtonArg;
using SerialCommandGrammar::parseInt;
using SerialCommandGrammar::PressKind;
using SerialCommandGrammar::ReplyLine;

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
}

TEST(SerialCommandGrammar, ParseIntRefusesJunkAndOverflow) {
  int v = 5;
  EXPECT_FALSE(parseInt("", v));
  EXPECT_FALSE(parseInt("-", v));
  EXPECT_FALSE(parseInt("12a", v));
  EXPECT_FALSE(parseInt(" 12", v));
  EXPECT_FALSE(parseInt("2147483648", v));
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
