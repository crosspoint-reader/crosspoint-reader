#include <HalClock.h>
#include <gtest/gtest.h>

namespace {

constexpr unsigned long POLL_INTERVAL_MS = 10000;

Rtc::DateTime timeOfDay(uint8_t hour, uint8_t minute) {
  Rtc::DateTime dt;
  dt.year = 2026;
  dt.month = 9;
  dt.day = 11;
  dt.hour = hour;
  dt.minute = minute;
  return dt;
}

class HalClockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fakeRtc = FakeRtc{};
    fakeMillis = 1000;
    testClock.begin();
  }

  void advance(unsigned long ms) { fakeMillis += ms; }

  HalClock testClock;
  uint8_t hour = 0;
  uint8_t minute = 0;
};

}  // namespace

TEST_F(HalClockTest, ReturnsTimeReadFromRtc) {
  fakeRtc.time = timeOfDay(14, 5);

  ASSERT_TRUE(testClock.getTime(hour, minute));
  EXPECT_EQ(hour, 14);
  EXPECT_EQ(minute, 5);
}

TEST_F(HalClockTest, HidesTimeWhenRtcBecomesUnreadable) {
  fakeRtc.time = timeOfDay(14, 5);
  ASSERT_TRUE(testClock.getTime(hour, minute));

  fakeRtc.readSucceeds = false;
  advance(POLL_INTERVAL_MS);

  EXPECT_FALSE(testClock.getTime(hour, minute));
}

TEST_F(HalClockTest, HidesTimeWhenRtcRegistersAreZeroed) {
  fakeRtc.time = Rtc::DateTime{};

  EXPECT_FALSE(testClock.getTime(hour, minute));
}

TEST_F(HalClockTest, ShowsTimeAgainOnceRtcIsReadable) {
  fakeRtc.readSucceeds = false;
  ASSERT_FALSE(testClock.getTime(hour, minute));

  fakeRtc.readSucceeds = true;
  fakeRtc.time = timeOfDay(9, 30);
  advance(POLL_INTERVAL_MS);

  ASSERT_TRUE(testClock.getTime(hour, minute));
  EXPECT_EQ(hour, 9);
  EXPECT_EQ(minute, 30);
}

TEST_F(HalClockTest, ReadsRtcAtMostOncePerPollInterval) {
  fakeRtc.time = timeOfDay(14, 5);
  testClock.getTime(hour, minute);

  advance(POLL_INTERVAL_MS - 1);
  testClock.getTime(hour, minute);
  EXPECT_EQ(fakeRtc.readCount, 1);

  advance(1);
  testClock.getTime(hour, minute);
  EXPECT_EQ(fakeRtc.readCount, 2);
}

TEST_F(HalClockTest, FormatTimeFailsWhenRtcTimeIsInvalid) {
  fakeRtc.readSucceeds = false;
  char buf[9];

  EXPECT_FALSE(testClock.formatTime(buf, sizeof(buf)));
}
