#include "HalTiltSensor.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <iterator>

#include "HalTiltSensor_IMUTiltEstimator.h"

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::readGyro(float& ax, float& ay, float& az, float& gx, float& gy, float& gz) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  ax = sample.ax;
  ay = sample.ay;
  az = sample.az;
  gx = sample.gx;
  gy = sample.gy;
  gz = sample.gz;
  return true;
}

void HalTiltSensor::begin() {
  _havePointerGyroBias = false;
  std::fill(std::begin(_pointerGyroBias), std::end(_pointerGyroBias), 0.0f);
  _available = _sdkImu.begin();
  if (_available) {
    _initMs = millis();
    _lastPollMs = millis();
    // begin() leaves the sensors sampling; stand them by until usage
    // actually wakes them, so a disabled IMU doesn't drain the battery.
    if (!_sdkImu.sleep()) {
      LOG_ERR("GYR", "IMU standby failed");
    }
    LOG_INF("GYR", "SDK IMU initialized");
    return;
  }
  LOG_ERR("GYR", "SDK IMU not found");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation) {
  if (!(mode & CrossPointTiltSensorMode::TILT_PAGE_ACTIVE)) {
    _tiltForwardEvent = false;
    _tiltBackEvent = false;
  }
  if (mode != _lastMode || orientation != _lastOrientation) {
    _pointerMoveX = 0;
    _pointerMoveY = 0;
  }
  _lastMode = mode;
  _lastOrientation = orientation;

  if (!_available) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if ((mode != CrossPointTiltPageTurn::TILT_OFF) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF) && _isAwake) {
    _isAwake = !deepSleep();
    if (_tiltEstimator) {
      delete _tiltEstimator;
      _tiltEstimator = nullptr;
    }
    _tiltEstimatorAllocationFailed = false;
    return;
  }

  // If disabled, skip the rest of the polling logic and avoid unnecessary I2C traffic in non-reader activities
  if ((mode == CrossPointTiltSensorMode::SENSOR_OFF)) {
    return;
  }

  if (mode & CrossPointTiltSensorMode::TILT_POINTER_ACTIVE) {
    if (!_tiltEstimator && !_tiltEstimatorAllocationFailed) {
      auto tiltEstimator = makeUniqueNoThrow<IMUTiltEstimator>();
      if (!tiltEstimator) {
        LOG_ERR("GYR-IMU", "OOM: IMUTiltEstimator");
        _tiltEstimatorAllocationFailed = true;
      } else {
        _tiltEstimator = tiltEstimator.release();
        _tiltEstimator->begin(_havePointerGyroBias ? _pointerGyroBias : nullptr);
      }
    }
  } else {
    if (_tiltEstimator) {
      delete _tiltEstimator;
      _tiltEstimator = nullptr;
    }
    _tiltEstimatorAllocationFailed = false;
    _pointerMoveX = 0;
    _pointerMoveY = 0;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  // accurate pointer navigation via tilt sensor requires more frequent imu updates
  if ((now - _lastPollMs) <
      ((mode & CrossPointTiltSensorMode::TILT_POINTER_ACTIVE) ? POLL_INTERVAL_FAST_MS : POLL_INTERVAL_MS)) {
    return;
  }
  _lastPollMs = now;

  float ax, ay, az, gx, gy, gz;
  if (!readGyro(ax, ay, az, gx, gy, gz)) {
    return;
  }

  if (mode & CrossPointTiltSensorMode::TILT_PAGE_ACTIVE) {
    // Map the gyro axis to left/right tilt based on reader orientation.
    // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
    float tiltAxis;
    switch (orientation) {
      case CrossPointOrientation::PORTRAIT:
        tiltAxis = (mode & CrossPointTiltPageTurn::TILT_INVERTED) ? -gx : gx;
        break;
      case CrossPointOrientation::INVERTED:
        tiltAxis = (mode & CrossPointTiltPageTurn::TILT_INVERTED) ? gx : -gx;
        break;
      case CrossPointOrientation::LANDSCAPE_CW:
        tiltAxis = (mode & CrossPointTiltPageTurn::TILT_INVERTED) ? gy : -gy;
        break;
      case CrossPointOrientation::LANDSCAPE_CCW:
        tiltAxis = (mode & CrossPointTiltPageTurn::TILT_INVERTED) ? -gy : gy;
        break;
      default:
        tiltAxis = gx;
        break;
    }

    if (_inTilt) {
      // Wait for device to return to neutral before allowing next trigger
      if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
        _inTilt = false;
      }
    } else {
      // Check for new tilt gesture (with cooldown)
      if ((now - _lastTiltMs) >= COOLDOWN_MS) {
        if (tiltAxis > RATE_THRESHOLD_DPS) {
          _tiltForwardEvent = true;
          _hadActivity = true;
          _inTilt = true;
          _lastTiltMs = now;
          LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
        } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
          _tiltBackEvent = true;
          _hadActivity = true;
          _inTilt = true;
          _lastTiltMs = now;
          LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
        }
      }
    }
  }

  if (mode & CrossPointTiltSensorMode::TILT_POINTER_ACTIVE && _tiltEstimator) {
    _tiltEstimator->consume(ax, ay, az, gx, gy, gz, now);
    if (_tiltEstimator->hasGyroBias()) {
      for (uint8_t axis = 0; axis < 3; ++axis) {
        _pointerGyroBias[axis] = _tiltEstimator->estimatedGyroBias(axis);
      }
      _havePointerGyroBias = true;
    }
    int moveX = 0;
    int moveY = 0;
    _tiltEstimator->pollPointerMove(moveX, moveY, orientation,
                                    (mode & CrossPointTiltSensorMode::TILT_POINTER_SENSITIVITY_LOW)    ? 0
                                    : (mode & CrossPointTiltSensorMode::TILT_POINTER_SENSITIVITY_HIGH) ? 2
                                                                                                       : 1,
                                    (mode & CrossPointTiltSensorMode::TILT_POINTER_INVERT_X) != 0,
                                    (mode & CrossPointTiltSensorMode::TILT_POINTER_INVERT_Y) != 0);
    if (moveX || moveY) {
      _pointerMoveX = static_cast<int8_t>(moveX);
      _pointerMoveY = static_cast<int8_t>(moveY);
      _hadActivity = true;
    }
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

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  _pointerMoveX = 0;
  _pointerMoveY = 0;
  if (_tiltEstimator) {
    delete _tiltEstimator;
    _tiltEstimator = nullptr;
  }
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}

bool HalTiltSensor::getXYPointerMove(int& moveX, int& moveY) {
  moveX = _pointerMoveX;
  moveY = _pointerMoveY;
  _pointerMoveX = 0;
  _pointerMoveY = 0;
  return moveX || moveY;
}
