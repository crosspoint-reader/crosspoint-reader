// The serial command lines are put together from whatever bytes have arrived, so the loop never
// waits for the rest of a line: a stray byte used to stop it for the 1 s Stream timeout, and a line
// typed a key at a time must still arrive whole.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/SerialLineBuffer.h"

namespace {

// Feeds `bytes` and collects every line they complete, as the loop does over its passes.
std::vector<std::string> feedAll(SerialLineBuffer& buffer, const std::string& bytes) {
  std::vector<std::string> lines;
  for (const char c : bytes) {
    if (buffer.feed(c)) lines.push_back(buffer.take());
  }
  return lines;
}

}  // namespace

TEST(SerialLineBuffer, ALineTypedAKeyAtATimeArrivesWhole) {
  SerialLineBuffer buffer;
  const std::string typed = "CMD:STATE";
  for (const char c : typed) {
    EXPECT_FALSE(buffer.feed(c)) << "a line is not complete before its end";
  }
  ASSERT_TRUE(buffer.feed('\n'));
  EXPECT_EQ(buffer.take(), "CMD:STATE");
}

TEST(SerialLineBuffer, AStrayByteWithoutALineEndIsNoLine) {
  SerialLineBuffer buffer;
  EXPECT_TRUE(feedAll(buffer, std::string(1, '\0')).empty());
  EXPECT_TRUE(feedAll(buffer, "x").empty());
}

TEST(SerialLineBuffer, CrLfAndCrlfEachEndOneLine) {
  for (const char* end : {"\n", "\r", "\r\n"}) {
    SerialLineBuffer buffer;
    const auto lines = feedAll(buffer, std::string("CMD:BOOTLOG") + end + "CMD:STATE" + end);
    ASSERT_EQ(lines.size(), 2u) << "line end of " << std::string(end).size() << " bytes";
    EXPECT_EQ(lines[0], "CMD:BOOTLOG");
    EXPECT_EQ(lines[1], "CMD:STATE");
  }
}

TEST(SerialLineBuffer, BytesAfterALineEndStartTheNextLine) {
  SerialLineBuffer buffer;
  // The loop stops reading at the first line end and leaves the rest for the next pass.
  const std::string sent = "CMD:BTN:DOWN\nCMD:ST";
  size_t i = 0;
  while (!buffer.feed(sent[i])) i++;
  EXPECT_EQ(buffer.take(), "CMD:BTN:DOWN");
  for (i++; i < sent.size(); i++) EXPECT_FALSE(buffer.feed(sent[i]));
  EXPECT_EQ(feedAll(buffer, "ATE\n"), std::vector<std::string>{"CMD:STATE"});
}

TEST(SerialLineBuffer, ALongPathFits) {
  SerialLineBuffer buffer;
  const std::string line = "CMD:ACT:READER:/" + std::string(SerialLineBuffer::MAX_LINE_BYTES - 16, 'a');
  ASSERT_EQ(line.size(), SerialLineBuffer::MAX_LINE_BYTES);
  EXPECT_EQ(feedAll(buffer, line + "\n"), std::vector<std::string>{line});
}

TEST(SerialLineBuffer, AnOverlongLineIsDroppedUpToItsEnd) {
  SerialLineBuffer buffer;
  const std::string noise(SerialLineBuffer::MAX_LINE_BYTES + 1, 'z');
  EXPECT_TRUE(feedAll(buffer, noise + "CMD:STATE\n").empty()) << "the tail of the long line is part of it";
  EXPECT_EQ(feedAll(buffer, "CMD:STATE\n"), std::vector<std::string>{"CMD:STATE"});
}

TEST(SerialLineBuffer, ResetDropsAPartLine) {
  SerialLineBuffer buffer;
  EXPECT_TRUE(feedAll(buffer, "CMD:SCREEN").empty());
  buffer.reset();
  EXPECT_EQ(feedAll(buffer, "CMD:STATE\n"), std::vector<std::string>{"CMD:STATE"});
}
