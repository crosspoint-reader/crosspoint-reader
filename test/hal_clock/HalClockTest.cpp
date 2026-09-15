#include <HalClock.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <gtest/gtest.h>
#include <sys/time.h>

#include <cstdlib>
#include <ctime>
#include <string>

namespace {
constexpr time_t VALID_EPOCH = 1709210096;  // 2024-02-29 12:34:56 UTC
struct ClockState {
  time_t epoch = 0;
  unsigned milliseconds = 0;
  bool rtcAvailable = true;
  bool rtcReadable = true;
  bool rtcWritable = true;
  bool systemWritable = true;
  Rtc::DateTime rtc{2024, 2, 29, 12, 34, 56, 4};
  Rtc::DateTime written;
  unsigned rtcWrites = 0;
  unsigned systemWrites = 0;
  int wifi = WL_CONNECTED;
  unsigned starts = 0;
  unsigned replyAt = 100;
  time_t replyEpoch = VALID_EPOCH;
  sntp_sync_status_t status = SNTP_SYNC_STATUS_RESET;
  std::string timezone;
} fake;

class HalClockTest : public ::testing::Test {
 protected:
  HalClock clock;
  void SetUp() override { fake = {}; }
};

TEST_F(HalClockTest, RestoresSystemUtcFromRtcOnBoot) {
  clock.begin();
  EXPECT_EQ(fake.epoch, VALID_EPOCH);
  EXPECT_EQ(fake.systemWrites, 1U);
  EXPECT_EQ(fake.starts, 0U);
}

TEST_F(HalClockTest, PreservesValidSystemTime) {
  fake.epoch = VALID_EPOCH + 1000;
  clock.begin();
  EXPECT_EQ(fake.epoch, VALID_EPOCH + 1000);
  EXPECT_EQ(fake.systemWrites, 0U);
}

TEST_F(HalClockTest, RestorationIgnoresHostTimezone) {
  const char* previous = std::getenv("TZ");
  const bool hadTimezone = previous != nullptr;
  const std::string saved = previous ? previous : "";
  setenv("TZ", "EST5EDT", 1);
  tzset();
  clock.begin();
  EXPECT_EQ(fake.epoch, VALID_EPOCH);
  if (hadTimezone)
    setenv("TZ", saved.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();
}

TEST_F(HalClockTest, RejectsInvalidRtcFields) {
  constexpr Rtc::DateTime cases[] = {
      {2019, 12, 31, 23, 59, 59, 2}, {2100, 1, 1, 0, 0, 0, 5},  {2024, 0, 1, 0, 0, 0, 1},  {2024, 13, 1, 0, 0, 0, 1},
      {2024, 1, 0, 0, 0, 0, 1},      {2024, 1, 32, 0, 0, 0, 1}, {2023, 2, 29, 0, 0, 0, 1}, {2024, 2, 30, 0, 0, 0, 1},
      {2024, 4, 31, 0, 0, 0, 1},     {2024, 1, 1, 24, 0, 0, 1}, {2024, 1, 1, 0, 60, 0, 1}, {2024, 1, 1, 0, 0, 60, 1}};
  for (const auto& date : cases) {
    fake = {};
    fake.rtc = date;
    HalClock instance;
    instance.begin();
    EXPECT_EQ(fake.systemWrites, 0U);
    EXPECT_EQ(fake.epoch, 0);
  }
}

TEST_F(HalClockTest, AcceptsRtcRangeBoundaries) {
  fake.rtc = {2020, 1, 1, 0, 0, 0, 3};
  clock.begin();
  EXPECT_EQ(fake.epoch, 1577836800);
  fake = {};
  fake.rtc = {2099, 12, 31, 23, 59, 59, 4};
  HalClock lastSecond;
  lastSecond.begin();
  EXPECT_EQ(fake.epoch, 4102444799);
}

TEST_F(HalClockTest, AbsentOrUnreadableRtcDoesNotSetSystemTime) {
  fake.rtcReadable = false;
  clock.begin();
  EXPECT_EQ(fake.systemWrites, 0U);
  fake = {};
  fake.rtcAvailable = false;
  HalClock absent;
  absent.begin();
  EXPECT_EQ(fake.systemWrites, 0U);
}

TEST_F(HalClockTest, FailedSystemClockWriteLeavesClockInvalid) {
  fake.systemWritable = false;
  clock.begin();
  EXPECT_EQ(fake.epoch, 0);
}

TEST_F(HalClockTest, NtpWorksWithoutRtc) {
  fake.rtcAvailable = false;
  clock.begin();
  EXPECT_TRUE(clock.syncFromNTP());
  EXPECT_EQ(fake.epoch, VALID_EPOCH);
  EXPECT_EQ(fake.rtcWrites, 0U);
  EXPECT_EQ(fake.timezone, "UTC0");
}

TEST_F(HalClockTest, NtpWritesRtcUtcAndUpdatesDisplayCache) {
  clock.begin();
  EXPECT_TRUE(clock.syncFromNTP());
  EXPECT_EQ(fake.rtcWrites, 1U);
  EXPECT_EQ(fake.written.year, 2024);
  EXPECT_EQ(fake.written.month, 2);
  EXPECT_EQ(fake.written.day, 29);
  EXPECT_EQ(fake.written.hour, 12);
  EXPECT_EQ(fake.written.minute, 34);
  EXPECT_EQ(fake.written.weekday, 4);
  fake.rtcReadable = false;
  char formatted[9] = {};
  EXPECT_TRUE(clock.formatTime(formatted, sizeof(formatted)));
  EXPECT_STREQ(formatted, "12:34");
}

TEST_F(HalClockTest, RtcWriteFailureRemainsFailureDespiteValidSystemTime) {
  fake.rtcWritable = false;
  clock.begin();
  EXPECT_FALSE(clock.syncFromNTP());
  EXPECT_EQ(fake.epoch, VALID_EPOCH);
  EXPECT_EQ(fake.rtcWrites, 1U);
}

TEST_F(HalClockTest, IgnoresStaleSntpCompletion) {
  clock.begin();
  fake.status = SNTP_SYNC_STATUS_COMPLETED;
  fake.replyAt = 6000;
  EXPECT_FALSE(clock.syncFromNTP());
  EXPECT_EQ(fake.milliseconds, 5000U);
  EXPECT_EQ(fake.rtcWrites, 0U);
}

TEST_F(HalClockTest, InvalidNtpDateCannotOverwriteRtc) {
  clock.begin();
  fake.replyEpoch = 0;
  EXPECT_FALSE(clock.syncFromNTP());
  EXPECT_EQ(fake.rtcWrites, 0U);
}

TEST_F(HalClockTest, DisconnectedWifiDoesNotStartNtp) {
  clock.begin();
  fake.wifi = WL_DISCONNECTED;
  EXPECT_FALSE(clock.syncFromNTP());
  EXPECT_EQ(fake.starts, 0U);
  EXPECT_EQ(fake.milliseconds, 0U);
}
}  // namespace

WiFiClass WiFi;
int WiFiClass::status() const { return fake.wifi; }
unsigned long millis() { return fake.milliseconds; }
void delay(unsigned long milliseconds) {
  fake.milliseconds += milliseconds;
  if (fake.starts && fake.milliseconds >= fake.replyAt) {
    fake.status = SNTP_SYNC_STATUS_COMPLETED;
    fake.epoch = fake.replyEpoch;
  }
}
void configTzTime(const char* timezone, const char*, const char*, const char*) {
  ++fake.starts;
  fake.timezone = timezone;
}
sntp_sync_status_t sntp_get_sync_status() { return fake.status; }
void sntp_set_sync_status(sntp_sync_status_t status) { fake.status = status; }
bool Rtc::begin() { return fake.rtcAvailable; }
bool Rtc::now(DateTime& output) {
  if (!fake.rtcReadable) return false;
  output = fake.rtc;
  return true;
}
bool Rtc::set(const DateTime& value) {
  ++fake.rtcWrites;
  fake.written = value;
  return fake.rtcWritable;
}
extern "C" time_t __wrap_time(time_t* output) {
  if (output) *output = fake.epoch;
  return fake.epoch;
}
extern "C" int __wrap_settimeofday(const timeval* value, const struct timezone*) {
  ++fake.systemWrites;
  if (!fake.systemWritable) return -1;
  fake.epoch = value->tv_sec;
  return 0;
}
