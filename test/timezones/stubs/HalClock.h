#pragma once

struct HalClock {
  void setTimezone(const char*) {}
};
extern HalClock halClock;
