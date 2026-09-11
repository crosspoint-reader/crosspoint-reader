#include "HalTiltSensor_IMUPitchRollEstimator.h"

#include <Arduino.h>
#include <Logging.h>

#include <numbers>

#include "HalTiltSensor.h"

#define INV_M_PI_F (std::numbers::inv_pi_v<float>)

void HalTiltSensor::IMUPitchRollEstimator::begin() {
  _state = CALIBRATING;
  _pifCalibSteps = 0;
  _lastPIFUpdateMs = 0;
  _pifBias[0] = _pifBias[1] = _pifEstimate[0] = _pifEstimate[1] = .0f;
  _resetTare();
}

void HalTiltSensor::IMUPitchRollEstimator::_resetTare() {
  memset(_mTare, 0, sizeof(_mTare));
  _mTare[0] = _mTare[4] = _mTare[8] = 1.0f;
  _pitch = _roll = _accRotation[0] = _accRotation[1] = 0.f;
}

bool HalTiltSensor::IMUPitchRollEstimator::_tare(float ax, float ay, float az, float gx, float gy, float gz) {
  gx -= _pifBias[0];
  gy -= _pifBias[1];

  if (fabsf(gx) > GYRO_ANGULAR_THRESH || fabsf(gy) > GYRO_ANGULAR_THRESH || fabsf(gz) > GYRO_ANGULAR_THRESH) {
    // too much angular noise/movement for now, try again later.
    return false;
  }

  const float len = sqrtf(ax * ax + ay * ay + az * az);

  if (len < 0.8 || len > 1.2f) {
    // accelerometer vector length should be around 1g when held still, so if we're outside that range, we assume motion
    // on axes.
    return false;
  }

  LOG_INF("GYR-PTR", "Run tare: ax:%.4f ay:%.4f az:%.4f gx:%.4f gy:%.4f gz:%.4f accLen:%.4f", ax, ay, az, gx, gy, gz,
          len);

  const float x = ax / len, y = ay / len, z = az / len;
  const float f = (1.0f - z) / (x * x + y * y);

  _mTare[0] = 1.0f - x * x * f;
  _mTare[1] = -x * y * f;
  _mTare[2] = -x;
  _mTare[3] = -x * y * f;
  _mTare[4] = 1.0f - y * y * f;
  _mTare[5] = -y;
  _mTare[6] = x;
  _mTare[7] = y;
  _mTare[8] = z;

  _pitch = _roll = _accRotation[0] = _accRotation[1] = 0.f;

  return true;
}

void HalTiltSensor::IMUPitchRollEstimator::_applyTare(float& x, float& y, float& z) {
  float _x = x, _y = y, _z = z;
  x = _mTare[0] * _x + _mTare[1] * _y + _mTare[2] * _z;
  y = _mTare[3] * _x + _mTare[4] * _y + _mTare[5] * _z;
  z = _mTare[6] * _x + _mTare[7] * _y + _mTare[8] * _z;
}

static inline float __to180(float a) { return (a > 90.0f) ? a - 180.0f : (a < -90.0f) ? a + 180.0f : a; }

void HalTiltSensor::IMUPitchRollEstimator::_accelPitchRoll(float& pitch, float& roll, float ax, float ay, float az) {
  pitch = __to180(atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f * INV_M_PI_F);
  roll = __to180(atan2f(ay, az) * 180.0f * INV_M_PI_F);
}

void HalTiltSensor::IMUPitchRollEstimator::_updatePIF(float ax, float ay, float az, float gx, float gy, float gz,
                                                      unsigned long timestamp) {
  if (0 == _lastPIFUpdateMs) {
    _pifBias[0] = _pifBias[1] = .0f;
    _accelPitchRoll(_pifEstimate[1], _pifEstimate[0], ax, ay, az);
    _lastPIFUpdateMs = timestamp;
    return;
  }

  const float dt = ((float)(timestamp - _lastPIFUpdateMs)) / 1000.0f;
  float ki = PIF_KI_NORMAL;

  switch (_state) {
    case CALIBRATING:
      ki = PIF_KI_BOOST -
           (PIF_KI_BOOST - PIF_KI_NORMAL) * (((float)_pifCalibSteps) * (1.0f / ((float)CALIBRATION_STEPS)));
      break;
    case NEEDS_TARE:
      ki = PIF_KI_NORMAL + .25f * (PIF_KI_BOOST - PIF_KI_NORMAL);
      break;
    default:
      break;
  }

  float p, r;
  _accelPitchRoll(p, r, ax, ay, az);

  const float gxr = gx - _pifBias[0];
  const float gyr = gy - _pifBias[1];

  const float rPred = _pifEstimate[0] + gxr * dt;
  const float pPred = _pifEstimate[1] + gyr * dt;

  const float rErr = r - rPred;
  const float pErr = p - pPred;

  _pifBias[0] -= ki * rErr * dt;
  _pifBias[1] -= ki * pErr * dt;

  _pifEstimate[0] = rPred + PIF_KP * rErr * dt;
  _pifEstimate[1] = pPred + PIF_KP * pErr * dt;

  _lastPIFUpdateMs = timestamp;
}

void HalTiltSensor::IMUPitchRollEstimator::_updateAngles(float ax, float ay, float az, float gx, float gy, float gz,
                                                         unsigned long timestamp) {
  const float dt = _lastAngleUpdateMs > 0 ? ((float)(timestamp - _lastAngleUpdateMs)) / 1000.0f : .0;

  _lastAngleUpdateMs = timestamp;

  gx -= _pifBias[0];
  gy -= _pifBias[1];

  _applyTare(ax, ay, az);
  _applyTare(gx, gy, gz);

  gx = fabsf(gx) > GYRO_ANGULAR_THRESH ? gx : .0f;
  gy = fabsf(gy) > GYRO_ANGULAR_THRESH ? gy : .0f;

  _accRotation[0] += gx * dt;
  _accRotation[1] += gy * dt;

  if (fabsf(_accRotation[0]) > GYRO_TARE_MAX_ANG || fabsf(_accRotation[1]) > GYRO_TARE_MAX_ANG) {
    // accumulated rotation since tareing exceeds threshold, so let's recalibrate
    _pitch = _roll = .0f;
    _state = NEEDS_TARE;
    return;
  }

  float aPitch, aRoll;
  _accelPitchRoll(aPitch, aRoll, ax, ay, az);

  if (0 == dt) {
    _pitch = aPitch;
    _roll = aRoll;
  } else {
    _pitch = COMPLEMENTARY_FACTOR * (_pitch + gy * dt) + (1.0f - COMPLEMENTARY_FACTOR) * aPitch;
    _roll = COMPLEMENTARY_FACTOR * (_roll + gx * dt) + (1.0f - COMPLEMENTARY_FACTOR) * aRoll;
  }
}

void HalTiltSensor::IMUPitchRollEstimator::_getPointerMove(float angle, int& dir, unsigned long& lastMillis,
                                                           bool invert, const float sensitivityFactor) {
  const float absAngle = fabsf(angle);
  const float minAngle = TRACK_MIN_ANGLE * sensitivityFactor;

  // The angle-to-direction (e.g. the sign) is chosen so that on an XTEINK X3, the "normal" (e.g. non-inverted)
  // configuration is correct with respect to the IMU's orientation relative to the screen. This may not hold for all
  // readers with a built-in IMU, but this can be alleviated with the inversion settings.
  const int m = absAngle >= minAngle ? (angle > 0 ? -1 : 1) * (invert ? -1 : 1) : 0;

  // Optimization: To trigger multiple move events in the same direction by holding the tilt, we expect the direction of
  // that tilt not to change while the repeat delay elapses. So that we don't need another field to store the direction
  // of the last move, we use the LSB of lastMillis for that, since it's negligible for timing purposes.

  if (m && (0 == lastMillis || ((lastMillis & 1UL) != ((m > 0) ? 0 : 1)) ||
            ((_lastAngleUpdateMs - lastMillis) >
             (MOVE_REPEAT_MIN_MS *
              (1.0f + 2.0f * std::max(0.0f, (1.0f - (absAngle - minAngle) / (GYRO_TARE_MAX_ANG - minAngle)))))))) {
    dir = m;
    lastMillis = ((_lastAngleUpdateMs - 2) & ~1UL) | ((m > 0) ? 0 : 1);
  } else {
    dir = 0;
    if (0 == m) {
      lastMillis = 0;
    }
  }
}

void HalTiltSensor::IMUPitchRollEstimator::consume(float ax, float ay, float az, float gx, float gy, float gz,
                                                   unsigned long timestamp) {
  LOG_DBG("GYR-PTR", "ax:%.4f ay:%.4f az:%.4f gx:%.4f gy:%.4f gz:%.4f bias_gx:%.4f bias_gy:%.4f pitch:%.4f roll:%.4f",
          ax, ay, az, gx, gy, gz, _pifBias[0], _pifBias[1], _pitch, _roll);

  _azNeg = az < .0f ? 1 : 0;

  switch (_state) {
    case CALIBRATING: {
      if (_pifCalibSteps > CALIBRATION_STEPS) {
        _state = NEEDS_TARE;
        LOG_INF("GYR-PTR", "Calibration done, gx bias: %.4f, gy bias: %.4f", _pifBias[0], _pifBias[1]);
      } else {
        _updatePIF(ax, ay, az, gx, gy, gz, timestamp);
        _pifCalibSteps++;
      }
      break;
    }

    case NEEDS_TARE: {
      _updatePIF(ax, ay, az, gx, gy, gz, timestamp);
      if (_tare(ax, ay, az, gx, gy, gz)) {
        _state = TRACKING;
        _lastAngleUpdateMs = 0;
      }
      break;
    }

    case TRACKING: {
      _updatePIF(ax, ay, az, gx, gy, gz, timestamp);
      _updateAngles(ax, ay, az, gx, gy, gz, timestamp);
      break;
    }

    default:
      break;
  }
}

void HalTiltSensor::IMUPitchRollEstimator::pollPointerMove(int& moveX, int& moveY, uint8_t orientation,
                                                           uint8_t sensitivity, bool invertX, bool invertY) {
  if (TRACKING != _state) {
    moveX = moveY = 0;
    return;
  }

  bool isLandscape = false;
  float sensitivityFactor = SENSITIVITY_FACTOR_NORMAL;

  switch (sensitivity) {
    case 0:
      sensitivityFactor = SENSITIVITY_FACTOR_LOW;
      break;
    case 2:
      sensitivityFactor = SENSITIVITY_FACTOR_HIGH;
      break;
    default:
      break;
  }

  if (!_azNeg) {
    // if we're upside down (e.g. display facing downwards on X3), we need to invert tracking on the x-axis to remain
    // consistent in behavior
    invertX = !invertX;
  }

  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      _getPointerMove(_roll, moveX, _lastMoveXMs, !invertX, sensitivityFactor);
      _getPointerMove(_pitch, moveY, _lastMoveYMs, invertY, sensitivityFactor);
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      _getPointerMove(_pitch, moveX, _lastMoveXMs, !invertX, sensitivityFactor);
      _getPointerMove(_roll, moveY, _lastMoveYMs, !invertY, sensitivityFactor);
      isLandscape = true;
      break;
    case CrossPointOrientation::INVERTED:
      _getPointerMove(_roll, moveX, _lastMoveXMs, invertX, sensitivityFactor);
      _getPointerMove(_pitch, moveY, _lastMoveYMs, !invertY, sensitivityFactor);
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      _getPointerMove(_pitch, moveX, _lastMoveXMs, invertX, sensitivityFactor);
      _getPointerMove(_roll, moveY, _lastMoveYMs, invertY, sensitivityFactor);
      isLandscape = true;
      break;
  }

  if (moveX) {
    LOG_INF("GYR-PTR", "Pointer detected motion on X: %d (%s: %.4f)", moveX, isLandscape ? "pitch" : "roll",
            isLandscape ? _pitch : _roll);
  }
  if (moveY) {
    LOG_INF("GYR-PTR", "Pointer detected motion on Y: %d (%s: %.4f)", moveY, isLandscape ? "roll" : "pitch",
            isLandscape ? _roll : _pitch);
  }
}
