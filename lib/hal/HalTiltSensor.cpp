#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;

bool HalTiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool HalTiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(_i2cAddr, static_cast<uint8_t>(1));
  if (Wire.available() < 1) {
    return false;
  }
  *val = Wire.read();
  return true;
}

bool HalTiltSensor::readReg16LE(uint8_t reg, int16_t* val) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(_i2cAddr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *val = static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  return true;
}

bool HalTiltSensor::readAccel(float& ax, float& ay, float& az) const {
  int16_t rawAx = 0;
  int16_t rawAy = 0;
  int16_t rawAz = 0;
  if (!readReg16LE(REG_AX_L, &rawAx) || !readReg16LE(REG_AX_L + 2, &rawAy) || !readReg16LE(REG_AX_L + 4, &rawAz)) {
    return false;
  }

  constexpr float SCALE = 1.0f / 16384.0f;
  ax = rawAx * SCALE;
  ay = rawAy * SCALE;
  az = rawAz * SCALE;
  return true;
}

bool HalTiltSensor::readAccelGyro(float& ax, float& ay, float& az, float& gx, float& gy, float& gz) const {
  int16_t rawAx = 0;
  int16_t rawAy = 0;
  int16_t rawAz = 0;
  int16_t rawGx = 0;
  int16_t rawGy = 0;
  int16_t rawGz = 0;
  if (!readReg16LE(REG_AX_L, &rawAx) || !readReg16LE(REG_AX_L + 2, &rawAy) || !readReg16LE(REG_AX_L + 4, &rawAz) ||
      !readReg16LE(REG_GYRO_X_L, &rawGx) || !readReg16LE(REG_GYRO_Y_L, &rawGy) || !readReg16LE(REG_GYRO_Z_L, &rawGz)) {
    return false;
  }

  constexpr float ACC_SCALE = 1.0f / 16384.0f;
  constexpr float GYRO_SCALE = 512.0f / 32768.0f;
  ax = rawAx * ACC_SCALE;
  ay = rawAy * ACC_SCALE;
  az = rawAz * ACC_SCALE;
  gx = rawGx * GYRO_SCALE;
  gy = rawGy * GYRO_SCALE;
  gz = rawGz * GYRO_SCALE;
  return true;
}

void HalTiltSensor::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  uint8_t whoami = 0;
  _i2cAddr = TILT_I2C_ADDR;
  if (!readReg(REG_WHO_AM_I, &whoami) || whoami != TILT_WHO_AM_I_VALUE) {
    _i2cAddr = TILT_I2C_ADDR_ALT;
    if (!readReg(REG_WHO_AM_I, &whoami) || whoami != TILT_WHO_AM_I_VALUE) {
      LOG_INF("TILT", "QMI8658 IMU not found");
      _available = false;
      return;
    }
  }

  LOG_INF("TILT", "QMI8658 IMU found at 0x%02X", _i2cAddr);

  if (!writeReg(REG_CTRL1, 0x40) || !writeReg(REG_CTRL2, CTRL2_2G_125HZ) || !writeReg(REG_CTRL3, CTRL3_512DPS_125HZ) ||
      !writeReg(REG_CTRL7, CTRL7_ACCEL_GYRO_EN)) {
    LOG_INF("TILT", "QMI8658 register configuration failed");
    _available = false;
    return;
  }

  _available = true;
  _lastPollMs = millis();
  _lastTiltMs = millis();
  _lastKalmanMicros = 0;
  _filterInitialized = false;
  _filterStartMs = millis();
  LOG_INF("TILT", "QMI8658 accelerometer initialized (±2g, 125 Hz) and gyro enabled");
}

void HalTiltSensor::deepSleep() {
  if (!_available) {
    return;
  }
  clearPendingEvents();
  _inTilt = false;
  _filterInitialized = false;
  _lastKalmanMicros = 0;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
}

void HalTiltSensor::update(bool enabled, uint8_t mode, uint8_t orientation) {
  if (!enabled || !_available) {
    return;
  }

  const unsigned long now = millis();
  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  bool isAngleMode = mode == 2;
  float tiltValue = 0.0f;
  if (isAngleMode) {
    float ax, ay, az, gx, gy, gz;
    if (!readAccelGyro(ax, ay, az, gx, gy, gz)) {
      return;
    }

    const float accRoll = atan2(ay, sqrt(ax * ax + az * az)) * (180.0f / PI);
    const float accPitch = atan2(-ax, sqrt(ay * ay + az * az)) * (180.0f / PI);

    const unsigned long nowMicros = micros();
    const unsigned long deltaMicros = nowMicros - _lastKalmanMicros;
    float dt = 0.008f;
    if (_lastKalmanMicros != 0 && deltaMicros <= 150000u) {
      dt = static_cast<float>(deltaMicros) * 1e-6f;
    } else {
      _kalmanRoll.setAngle(accRoll);
      _kalmanPitch.setAngle(accPitch);
    }
    _lastKalmanMicros = nowMicros;

    const float stableRoll = _kalmanRoll.update(accRoll, gx, dt);
    const float stablePitch = _kalmanPitch.update(accPitch, gy, dt);

    switch (orientation) {
      case 0:  // PORTRAIT
        tiltValue = stablePitch;
        break;
      case 2:  // INVERTED
        tiltValue = -stablePitch;
        break;
      case 1:  // LANDSCAPE_CW
        tiltValue = stableRoll;
        break;
      case 3:  // LANDSCAPE_CCW
        tiltValue = -stableRoll;
        break;
      default:
        tiltValue = stablePitch;
        break;
    }
  } else {
    float ax, ay, az;
    if (!readAccel(ax, ay, az)) {
      return;
    }

    float tiltAxis;
    switch (orientation) {
      case 0:  // PORTRAIT
        tiltAxis = ay;
        break;
      case 2:  // INVERTED
        tiltAxis = -ay;
        break;
      case 1:  // LANDSCAPE_CW
        tiltAxis = ax;
        break;
      case 3:  // LANDSCAPE_CCW
        tiltAxis = -ax;
        break;
      default:
        tiltAxis = ay;
        break;
    }

    if (mode == 1) {
      if (!_filterInitialized) {
        _filteredAxis = tiltAxis;
        _filterInitialized = true;
        _filterStartMs = now;
      } else {
        _filteredAxis = _filteredAxis * (1.0f - FILTER_ALPHA) + tiltAxis * FILTER_ALPHA;
      }
      if ((now - _filterStartMs) < FILTER_WARMUP_MS) {
        return;
      }
      tiltAxis = _filteredAxis;
    }
    tiltValue = tiltAxis;
  }

  if (_inTilt) {
    const float neutralThreshold = isAngleMode ? NEUTRAL_THRESHOLD_DEG : NEUTRAL_THRESHOLD_G;
    if (fabsf(tiltValue) < neutralThreshold) {
      _inTilt = false;
    }
    return;
  }

  if ((now - _lastTiltMs) < COOLDOWN_MS) {
    return;
  }

  if (tiltValue > (isAngleMode ? TILT_THRESHOLD_DEG : TILT_THRESHOLD_G)) {
    _tiltForwardEvent = true;
    _inTilt = true;
    _hadActivity = true;
    _lastTiltMs = now;
  } else if (tiltValue < (isAngleMode ? -TILT_THRESHOLD_DEG : -TILT_THRESHOLD_G)) {
    _tiltBackEvent = true;
    _inTilt = true;
    _hadActivity = true;
    _lastTiltMs = now;
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}
