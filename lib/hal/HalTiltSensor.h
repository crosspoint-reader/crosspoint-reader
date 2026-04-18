#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

// TODO: Move enums into new header and share with CrossPointSettings.h
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _initMs = 0;       // Timestamp of sensor init
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown
  unsigned long _wakeMs = 0;       // Timestamp of last wake() for stabilization

  // Tuning constants
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;      // Deg/sec speed to trigger flick
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;         // Must stop moving below this rate before next trigger
  static constexpr unsigned long COOLDOWN_MS = 600;        // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;    // 20 Hz polling
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;  // Ignore readings after wake
  static constexpr unsigned long SLEEP_STABILIZE_MS = 15;  // Sleep turn on/off delay

  mutable unsigned long _lastPollMs = 0;

  // --- QMI8658 registers ---
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL3 = 0x04;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_GX_L = 0x3B;

  // --- Register Bit Flags ---

  // REG_CTRL1 (0x02)
  static constexpr uint8_t CTRL1_BIG_ENDIAN = (1 << 5);                     // 0x20: Default state (1 = Big Endian)
  static constexpr uint8_t CTRL1_AUTO_INC = (1 << 6);                       // 0x40: Enable address auto-increment
  static constexpr uint8_t CTRL1_SENSOR_DISABLE = (1 << 0);                 // 0x01: Power down sensor engine
  static constexpr uint8_t CTRL1_BASE = CTRL1_AUTO_INC | CTRL1_BIG_ENDIAN;  // 0x60

  // REG_CTRL3 (0x04) - Gyro Config
  static constexpr uint8_t CTRL3_FS_512DPS = (0b101 << 4);  // Bits 6:4 = 101
  static constexpr uint8_t CTRL3_ODR_28HZ = 0b1000;         // Bits 3:0 = 1000 (28.025 Hz)

  // REG_CTRL7 (0x08) - Enable
  static constexpr uint8_t CTRL7_DISABLE_ALL = 0x00;
  static constexpr uint8_t CTRL7_GYRO_ENABLE = (1 << 1);  // Bit 1 = 1

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readGyro(float& gx, float& gy, float& gz) const;

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // Enables the QMI8658 internal sensor engine
  bool wake();

  // Puts the QMI8658 into a low-power standby state
  bool deepSleep();

  // True if the QMI8658 IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update tilt gesture state.
  void update(const uint8_t mode, const uint8_t orientation, const bool inReader);

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
