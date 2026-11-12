// The serial replies' line framing (util/LineFraming.h) over a link whose writes can give up, as
// writeAll() does when the host stops reading.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/LineFraming.h"

namespace {

// Takes or refuses each write in turn (all taken once `results` runs out) and keeps what it took.
struct Link {
  std::vector<bool> results;
  std::string out;
  size_t calls = 0;

  bool write(const uint8_t* data, const size_t len) {
    const bool ok = calls < results.size() ? results[calls] : true;
    ++calls;
    if (ok) out.append(reinterpret_cast<const char*>(data), len);
    return ok;
  }
};

bool sendLine(LineFraming& framing, Link& link, const std::string& text) {
  return framing.sendLine([&link](const uint8_t* data, const size_t len) { return link.write(data, len); },
                          reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

}  // namespace

TEST(LineFraming, LinesGoOutAsTheyAre) {
  LineFraming framing;
  Link link;
  EXPECT_TRUE(sendLine(framing, link, "BTN_OK\n"));
  EXPECT_TRUE(sendLine(framing, link, "IDLE:250\n"));
  EXPECT_EQ(link.out, "BTN_OK\nIDLE:250\n");
}

TEST(LineFraming, AReplyAfterACutOneStartsOnANewLine) {
  LineFraming framing;
  Link link;
  link.results = {false};  // the first reply is given up part way
  EXPECT_FALSE(sendLine(framing, link, "STATE: activity=Home\n"));
  EXPECT_TRUE(sendLine(framing, link, "IDLE:250\n"));
  EXPECT_EQ(link.out, "\nIDLE:250\n");
}

// The line end that would close the cut line is refused too: the reply must not join that line.
TEST(LineFraming, AReplyIsNotSentWhenItsLineEndIsNot) {
  LineFraming framing;
  Link link;
  link.results = {false, false};  // the first reply, then the line end before the second
  EXPECT_FALSE(sendLine(framing, link, "STATE: activity=Home\n"));
  EXPECT_FALSE(sendLine(framing, link, "IDLE:250\n"));
  EXPECT_EQ(link.calls, 2u) << "the second reply itself was not written";
  EXPECT_TRUE(sendLine(framing, link, "BTN_OK\n"));
  EXPECT_EQ(link.out, "\nBTN_OK\n");
}

TEST(LineFraming, AnErrorAfterACutFrameStartsOnANewLine) {
  LineFraming framing;
  Link link;
  framing.markOpen();
  EXPECT_TRUE(sendLine(framing, link, "SCREENSHOT_ERROR:transfer stalled\n"));
  EXPECT_EQ(link.out, "\nSCREENSHOT_ERROR:transfer stalled\n");
}
