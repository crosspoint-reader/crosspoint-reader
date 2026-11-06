#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Base64Decoder.h"

namespace {

// Collects decoded bytes; also records how many callbacks fired (the batching
// property matters on-device: tiny callbacks would hammer the SD writer).
struct Sink {
  std::string bytes;
  int callbacks = 0;
  bool ok = true;

  bool write(const uint8_t* data, size_t len) {
    callbacks++;
    bytes.append(reinterpret_cast<const char*>(data), len);
    return ok;
  }
};

void feedAll(Base64Decoder& d, Sink& s, const std::string& in, size_t chunk) {
  for (size_t i = 0; i < in.size(); i += chunk) {
    d.feed(in.data() + i, static_cast<int>(std::min(chunk, in.size() - i)),
           [&s](const uint8_t* p, size_t n) { return s.write(p, n); });
  }
  d.finish([&s](const uint8_t* p, size_t n) { return s.write(p, n); });
}

}  // namespace

TEST(Base64Decoder, DecodesSimplePayload) {
  Base64Decoder d;
  Sink s;
  feedAll(d, s, "QUJD", 64);  // "ABC"
  EXPECT_EQ(s.bytes, "ABC");
  EXPECT_FALSE(d.failed());
}

TEST(Base64Decoder, PreservesPaddingAndWhitespace) {
  Base64Decoder d;
  Sink s;
  feedAll(d, s, "QUJD\nRA==\r\n ", 64);  // "ABCD" with whitespace padding forms
  EXPECT_EQ(s.bytes, "ABCD");
  EXPECT_FALSE(d.failed());
}

TEST(Base64Decoder, OneAndTwoBytePaddingGroups) {
  Base64Decoder d;
  Sink s;
  // Whitespace between padding forms is skipped, so the groups decode back to
  // back: "QQ==" -> "A", "QUI=" -> "AB".
  feedAll(d, s, "QQ== QUI=", 64);
  EXPECT_EQ(s.bytes, "AAB");
  EXPECT_FALSE(d.failed());
}

// Feeding 7 bytes at a time must still decode correctly (the decoder buffers
// partial groups across feeds). Callback granularity is one per 4-char group
// by design — batching into SD-sized writes is the writer's job (see
// StoredZipWriter's 4 KB IO buffer).
TEST(Base64Decoder, BatchesOutputAcrossTinyInputFeeds) {
  Base64Decoder d;
  Sink s;
  std::string input(800, 'A');  // 800 chars -> 200 groups -> 600 bytes
  feedAll(d, s, input, 7);
  EXPECT_EQ(s.bytes.size(), 600u);
  EXPECT_FALSE(d.failed());
  EXPECT_EQ(s.callbacks, 200);
}

TEST(Base64Decoder, IncompleteGroupFails) {
  Base64Decoder d;
  Sink s;
  feedAll(d, s, "QUJ", 64);  // 3 chars: no complete group, no padding
  EXPECT_TRUE(d.failed());
}

TEST(Base64Decoder, InvalidCharacterFails) {
  Base64Decoder d;
  Sink s;
  feedAll(d, s, "QU*=", 64);
  EXPECT_TRUE(d.failed());
}

TEST(Base64Decoder, SinkRefusalFails) {
  Base64Decoder d;
  Sink s;
  s.ok = false;
  d.feed("QUJD", 4, [&s](const uint8_t* p, size_t n) { return s.write(p, n); });
  // 4 chars stay buffered (below the 256-byte flush): the failure surfaces at
  // finish(), when the group is actually decoded and written.
  EXPECT_FALSE(d.failed());
  d.finish([&s](const uint8_t* p, size_t n) { return s.write(p, n); });
  EXPECT_TRUE(d.failed());
}
