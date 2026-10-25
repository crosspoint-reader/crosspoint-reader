#include <gtest/gtest.h>

#include "SerialCommand.h"
#include "SerialLineBuffer.h"

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

TEST(SerialCommand, FileCommandsTakeAnAbsolutePath) {
  auto cmd = parseSerialCommand("LS /Books/Some Folder");
  EXPECT_EQ(cmd.type, Type::List);
  EXPECT_EQ(cmd.path, "/Books/Some Folder");
  EXPECT_EQ(parseSerialCommand("MKDIR /a/b").type, Type::MakeDir);
  EXPECT_EQ(parseSerialCommand("RM /a.epub").type, Type::Remove);
  EXPECT_EQ(parseSerialCommand("LS").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("RM a.epub").type, Type::Invalid);
}

TEST(SerialCommand, GetTakesAnOptionalOffset) {
  auto cmd = parseSerialCommand("GET /a b.txt");
  EXPECT_EQ(cmd.type, Type::Get);
  EXPECT_EQ(cmd.offset, 0u);
  EXPECT_EQ(cmd.path, "/a b.txt");

  cmd = parseSerialCommand("GET 8192 /a.txt");
  EXPECT_EQ(cmd.type, Type::Get);
  EXPECT_EQ(cmd.offset, 8192u);
  EXPECT_EQ(cmd.path, "/a.txt");

  EXPECT_EQ(parseSerialCommand("GET 12x /a.txt").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("GET 5").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("GET 99999999999 /a.txt").type, Type::Invalid);
}

TEST(SerialCommand, PutTakesSizeCrcAndPath) {
  auto cmd = parseSerialCommand("PUT 1234 cbf43926 /Books/A Book.epub");
  EXPECT_EQ(cmd.type, Type::Put);
  EXPECT_FALSE(cmd.replace);
  EXPECT_EQ(cmd.size, 1234u);
  EXPECT_EQ(cmd.crc, 0xCBF43926u);
  EXPECT_EQ(cmd.path, "/Books/A Book.epub");

  cmd = parseSerialCommand("PUT REPLACE 0 00000000 /empty.txt");
  EXPECT_EQ(cmd.type, Type::Put);
  EXPECT_TRUE(cmd.replace);
  EXPECT_EQ(cmd.size, 0u);
  EXPECT_EQ(cmd.crc, 0u);

  EXPECT_EQ(parseSerialCommand("PUT 4294967295 FFFFFFFF /big").size, 4294967295u);
  EXPECT_EQ(parseSerialCommand("PUT 4294967296 FFFFFFFF /big").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT 12 cbf4392 /short-crc").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT 12 cbf43926x /long-crc").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT -1 cbf43926 /neg").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT 12 cbf43926 relative").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT 12 cbf43926").type, Type::Invalid);
  EXPECT_EQ(parseSerialCommand("PUT REPLACE").type, Type::Invalid);
}

bool feedAll(SerialLineBuffer<16>& buffer, std::string_view bytes) {
  bool ended = false;
  for (const char byte : bytes) ended = buffer.feed(byte);
  return ended;
}

TEST(SerialLineBuffer, ALineEndsOnlyAtTheNewline) {
  SerialLineBuffer<16> buffer;
  EXPECT_FALSE(feedAll(buffer, "CMD:PI"));
  EXPECT_FALSE(feedAll(buffer, "NG"));
  EXPECT_TRUE(feedAll(buffer, "\n"));
  EXPECT_EQ(buffer.line(), "CMD:PING");
  EXPECT_FALSE(buffer.overflowed());
}

TEST(SerialLineBuffer, TheNextLineStartsClean) {
  SerialLineBuffer<16> buffer;
  ASSERT_TRUE(feedAll(buffer, "CMD:STATE\r\n"));
  EXPECT_EQ(buffer.line(), "CMD:STATE\r");  // the parser trims the CR
  ASSERT_TRUE(feedAll(buffer, "CMD:HOME\n"));
  EXPECT_EQ(buffer.line(), "CMD:HOME");
  ASSERT_TRUE(feedAll(buffer, "\n"));
  EXPECT_EQ(buffer.line(), "");
}

TEST(SerialLineBuffer, ALongLineKeepsItsStartAndIsFlagged) {
  SerialLineBuffer<16> buffer;
  ASSERT_TRUE(feedAll(buffer, "CMD:OPEN /a/very/long/path.epub\n"));
  EXPECT_TRUE(buffer.overflowed());
  EXPECT_EQ(buffer.line(), "CMD:OPEN /a/very");
  EXPECT_EQ(parseSerialCommand(buffer.line().substr(4)).name, "OPEN");

  ASSERT_TRUE(feedAll(buffer, "CMD:PING\n"));
  EXPECT_FALSE(buffer.overflowed());
  EXPECT_EQ(buffer.line(), "CMD:PING");
}

// The grammar is constexpr, so commands can be checked at compile time too.
static_assert(parseSerialCommand("TURN NEXT CHAPTER").chapter);

}  // namespace
