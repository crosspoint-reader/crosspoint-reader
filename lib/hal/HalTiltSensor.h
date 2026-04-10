#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _inTilt = false;            // Currently tilted past threshold
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown

  // Tuning constants
  static constexpr float TILT_THRESHOLD_G = 0.45f;       // ~27° tilt to trigger
  static constexpr float NEUTRAL_THRESHOLD_G = 0.25f;    // Must return below this before next trigger
  static constexpr unsigned long COOLDOWN_MS = 600;      // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;  // 20 Hz polling

  mutable unsigned long _lastPollMs = 0;

  // QMI8658 registers
  static constexpr uint8_t REG_WHO_AM_I = 0x00;
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL2 = 0x03;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_AX_L = 0x35;

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readAccel(float& ax, float& ay, float& az) const;

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the QMI8658 IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update tilt gesture state.
  // Should be called regularly in the main loop (internally rate-limited).
  void update();

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();
};
