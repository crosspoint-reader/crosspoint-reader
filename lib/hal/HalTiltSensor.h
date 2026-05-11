#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;
  bool _tiltForwardEvent = false;
  bool _tiltBackEvent = false;
  bool _inTilt = false;
  bool _hadActivity = false;
  bool _filterInitialized = false;
  float _filteredAxis = 0.0f;
  unsigned long _filterStartMs = 0;
  unsigned long _lastTiltMs = 0;
  unsigned long _lastKalmanMicros = 0;
  mutable unsigned long _lastPollMs = 0;

  static constexpr float TILT_THRESHOLD_G = 0.45f;     // ~27° tilt to trigger
  static constexpr float NEUTRAL_THRESHOLD_G = 0.25f;  // Must return below this before next trigger
  static constexpr float TILT_THRESHOLD_DEG = 27.0f;   // Angle threshold for fused tilt detection
  static constexpr float NEUTRAL_THRESHOLD_DEG = 15.0f;
  static constexpr unsigned long COOLDOWN_MS = 600;       // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;   // 20 Hz polling
  static constexpr float FILTER_ALPHA = 0.25f;            // Smoothing factor for axis stabilization
  static constexpr unsigned long FILTER_WARMUP_MS = 300;  // Stabilization warmup after first sample

  static constexpr uint8_t TILT_I2C_ADDR = 0x6A;
  static constexpr uint8_t TILT_I2C_ADDR_ALT = 0x6B;
  static constexpr uint8_t TILT_WHO_AM_I_VALUE = 0x05;
  static constexpr uint8_t REG_WHO_AM_I = 0x00;
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL2 = 0x03;
  static constexpr uint8_t REG_CTRL3 = 0x04;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_AX_L = 0x35;
  static constexpr uint8_t REG_GYRO_X_L = 0x3B;
  static constexpr uint8_t REG_GYRO_Y_L = 0x3D;
  static constexpr uint8_t REG_GYRO_Z_L = 0x3F;

  static constexpr uint8_t CTRL2_2G_125HZ = 0x06;
  static constexpr uint8_t CTRL3_512DPS_125HZ = 0x45;
  static constexpr uint8_t CTRL7_ACCEL_GYRO_EN = 0x03;

  struct KalmanFilter {
    KalmanFilter() {
      Q_angle = 0.001f;
      Q_bias = 0.003f;
      R_measure = 0.03f;
      angle = 0.0f;
      bias = 0.0f;
      P[0][0] = 0.0f;
      P[0][1] = 0.0f;
      P[1][0] = 0.0f;
      P[1][1] = 0.0f;
    }

    float update(float newAngle, float newRate, float dt) {
      float rate = newRate - bias;
      angle += dt * rate;

      P[0][0] += dt * (dt * P[1][1] - P[0][1] - P[1][0] + Q_angle);
      P[0][1] -= dt * P[1][1];
      P[1][0] -= dt * P[1][1];
      P[1][1] += Q_bias * dt;

      float S = P[0][0] + R_measure;
      float K0 = P[0][0] / S;
      float K1 = P[1][0] / S;

      float y = newAngle - angle;
      angle += K0 * y;
      bias += K1 * y;

      float P00_temp = P[0][0];
      float P01_temp = P[0][1];

      P[0][0] -= K0 * P00_temp;
      P[0][1] -= K0 * P01_temp;
      P[1][0] -= K1 * P00_temp;
      P[1][1] -= K1 * P01_temp;

      return angle;
    }

    void setAngle(float a) { angle = a; }

   private:
    float Q_angle;
    float Q_bias;
    float R_measure;
    float angle;
    float bias;
    float P[2][2];
  };

  KalmanFilter _kalmanRoll;
  KalmanFilter _kalmanPitch;

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readReg16LE(uint8_t reg, int16_t* val) const;
  bool readAccel(float& ax, float& ay, float& az) const;
  bool readAccelGyro(float& ax, float& ay, float& az, float& gx, float& gy, float& gz) const;

 public:
  void begin();
  void deepSleep();
  void update(bool enabled, uint8_t mode, uint8_t orientation);

  bool wasTiltedForward();
  bool wasTiltedBack();
  bool hadActivity();

  bool isAvailable() const { return _available; }
  void clearPendingEvents();
};
