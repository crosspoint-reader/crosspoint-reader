#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;

  bool _tiltForwardEvent = false;
  bool _tiltBackEvent = false;
  bool _hadActivity = false;
  bool _inTilt = false;
  bool _isAwake = false;
  unsigned long _initMs = 0;
  unsigned long _sleepMs = 0;
  unsigned long _lastTiltMs = 0;
  unsigned long _wakeMs = 0;

  static constexpr float RATE_THRESHOLD_DPS = 270.0f;
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;
  static constexpr unsigned long COOLDOWN_MS = 600;
  static constexpr unsigned long POLL_INTERVAL_MS = 50;
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;
  static constexpr unsigned long SLEEP_STABILIZE_MS = 15;

  mutable unsigned long _lastPollMs = 0;

  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL3 = 0x04;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_GX_L = 0x3B;

  static constexpr uint8_t CTRL1_SPI_BE = (1 << 5);  // SPI byte-order bit; no-op on I2C (QMI8658C)
  static constexpr uint8_t CTRL1_AUTO_INC = (1 << 6);
  static constexpr uint8_t CTRL1_SENSOR_DISABLE = (1 << 0);
  static constexpr uint8_t CTRL1_BASE = CTRL1_AUTO_INC | CTRL1_SPI_BE;

  static constexpr uint8_t CTRL3_FS_512DPS = (0b101 << 4);
  static constexpr uint8_t CTRL3_ODR_28HZ = 0b1000;

  static constexpr uint8_t CTRL7_DISABLE_ALL = 0x00;
  static constexpr uint8_t CTRL7_GYRO_ENABLE = (1 << 1);

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readGyro(float& gx, float& gy, float& gz) const;

 public:
  void begin();
  bool wake();
  bool deepSleep();

  bool isAvailable() const { return _available; }

  void update(CrossPointTiltPageTurn::Value mode, CrossPointOrientation::Value orientation, bool inReader);

  bool wasTiltedForward();
  bool wasTiltedBack();
  bool hadActivity();
  void clearPendingEvents();
};
