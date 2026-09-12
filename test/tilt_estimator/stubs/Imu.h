#pragma once

class Imu {
 public:
  struct Sample {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
  };

  inline static Sample nextSample{0, 0, 1, 0, 0, 0};
  inline static bool beginResult = true;
  inline static bool sleepResult = true;
  inline static bool wakeResult = true;
  inline static bool readResult = true;

  static void reset() {
    nextSample = {0, 0, 1, 0, 0, 0};
    beginResult = true;
    sleepResult = true;
    wakeResult = true;
    readResult = true;
  }

  bool begin() { return beginResult; }
  bool sleep() { return sleepResult; }
  bool wake() { return wakeResult; }
  bool read(Sample& sample) {
    if (!readResult) return false;
    sample = nextSample;
    return true;
  }
};
