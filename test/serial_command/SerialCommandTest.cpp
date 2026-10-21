#include <gtest/gtest.h>

#include "SerialCommand.h"

namespace {

using Type = SerialCommand::Type;

TEST(SerialCommand, BareCommandsTakeNoArgument) {
  EXPECT_EQ(parseSerialCommand("PING").type, Type::Ping);
  EXPECT_EQ(parseSerialCommand("STATE").type, Type::State);
  EXPECT_EQ(parseSerialCommand("SCREENSHOT").type, Type::Screenshot);
  EXPECT_EQ(parseSerialCommand("MENU").type, Type::Menu);
  EXPECT_EQ(parseSerialCommand("HOME").type, Type::Home);

  const auto cmd = parseSerialCommand("HOME now");
  EXPECT_EQ(cmd.type, Type::Invalid);
  EXPECT_EQ(cmd.name, "HOME");
  EXPECT_STREQ(cmd.error, "takes no argument");
}

TEST(SerialCommand, TerminalLineEndingsAndPaddingAreIgnored) {
  // Serial terminals send CRLF; readStringUntil('\n') leaves the CR.
  EXPECT_EQ(parseSerialCommand("PING\r").type, Type::Ping);
  EXPECT_EQ(parseSerialCommand("  STATE  \r\n").type, Type::State);
  const auto cmd = parseSerialCommand("CLICK   CONFIRM\r");
  EXPECT_EQ(cmd.type, Type::Click);
  EXPECT_EQ(cmd.click, RemoteButton::Confirm);
}

TEST(SerialCommand, ClickNamesEachRemoteButton) {
  EXPECT_EQ(parseSerialCommand("CLICK CONFIRM").click, RemoteButton::Confirm);
  EXPECT_EQ(parseSerialCommand("CLICK BACK").click, RemoteButton::Back);
  EXPECT_EQ(parseSerialCommand("CLICK NEXT").click, RemoteButton::Next);
  EXPECT_EQ(parseSerialCommand("CLICK PREV").click, RemoteButton::Previous);

  EXPECT_EQ(parseSerialCommand("CLICK").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("CLICK confirm").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("CLICK UP").type, Type::Invalid);
}

TEST(SerialCommand, TurnTakesADirectionAndOptionalChapter) {
  auto cmd = parseSerialCommand("TURN NEXT");
  EXPECT_EQ(cmd.type, Type::Turn);
  EXPECT_TRUE(cmd.forward);
  EXPECT_FALSE(cmd.chapter);

  cmd = parseSerialCommand("TURN PREV CHAPTER");
  EXPECT_EQ(cmd.type, Type::Turn);
  EXPECT_FALSE(cmd.forward);
  EXPECT_TRUE(cmd.chapter);

  EXPECT_EQ(parseSerialCommand("TURN").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("TURN CHAPTER").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("TURN NEXT PAGE").type, Type::Invalid);
}

TEST(SerialCommand, OpenKeepsThePathWholeAndAcceptsCold) {
  auto cmd = parseSerialCommand("OPEN /Books/A Long Title.epub\r");
  EXPECT_EQ(cmd.type, Type::Open);
  EXPECT_FALSE(cmd.cold);
  EXPECT_EQ(cmd.path, "/Books/A Long Title.epub");

  cmd = parseSerialCommand("OPEN COLD /a.epub");
  EXPECT_EQ(cmd.type, Type::Open);
  EXPECT_TRUE(cmd.cold);
  EXPECT_EQ(cmd.path, "/a.epub");
}

TEST(SerialCommand, OpenNeedsAnAbsolutePath) {
  EXPECT_EQ(parseSerialCommand("OPEN").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("OPEN COLD").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("OPEN a.epub").type, Type::Invalid);
  // A relative path that happens to start with the keyword is still relative.
  EXPECT_EQ(parseSerialCommand("OPEN COLDbook.epub").type, Type::Invalid);
}

TEST(SerialCommand, RestartTargets) {
  EXPECT_EQ(parseSerialCommand("RESTART").restart, SerialCommand::RestartTarget::Boot);
  EXPECT_EQ(parseSerialCommand("RESTART HOME").restart, SerialCommand::RestartTarget::Home);
  EXPECT_EQ(parseSerialCommand("RESTART READER").restart, SerialCommand::RestartTarget::Reader);
  EXPECT_EQ(parseSerialCommand("RESTART NOW").type, Type::Invalid);
}

TEST(SerialCommand, UnknownAndEmptyLinesAreRefusedByName) {
  auto cmd = parseSerialCommand("FLY away");
  EXPECT_EQ(cmd.type, Type::Invalid);
  EXPECT_EQ(cmd.name, "FLY");
  EXPECT_STREQ(cmd.error, "unknown command");

  cmd = parseSerialCommand("");
  EXPECT_EQ(cmd.type, Type::Invalid);
  EXPECT_EQ(cmd.name, "");
}

// The grammar is constexpr, so commands can be checked at compile time too.
static_assert(parseSerialCommand("TURN NEXT CHAPTER").chapter);

}  // namespace
