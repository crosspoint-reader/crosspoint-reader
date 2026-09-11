#pragma once

#include <cstdint>

class Rtc {
 public:
  struct DateTime {
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 0;
  };

  bool begin() { return true; }
  bool now(DateTime& out);
  bool set(const DateTime& dt);
};

struct FakeRtc {
  bool readSucceeds = true;
  Rtc::DateTime time;
  int readCount = 0;
};

inline FakeRtc fakeRtc;

inline bool Rtc::now(DateTime& out) {
  ++fakeRtc.readCount;
  if (!fakeRtc.readSucceeds) return false;
  out = fakeRtc.time;
  return true;
}

inline bool Rtc::set(const DateTime& dt) {
  fakeRtc.time = dt;
  return true;
}
