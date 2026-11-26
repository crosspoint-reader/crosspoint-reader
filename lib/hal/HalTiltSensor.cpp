#include "HalTiltSensor.h"

#include <BoardConfig.h>
#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

void HalTiltSensor::begin() {
  if (!_sdkImu.begin()) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }
  // begin() leaves the sensors sampling; stand them by until tilt page turn
  // actually wakes them, so a disabled IMU doesn't drain the battery.
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU standby failed");
  }
  // The Read Pico uses acceleration changes for its tilt gesture.
  _available = _sdkImu.hasGyroscope();
#if FREEINK_DEVICE_READPICO
  _available |= BoardConfig::isReadPico();
#endif
  if (!_available) {
    LOG_INF("GYR", "IMU has no gyroscope");
    return;
  }
  _initMs = millis();
  _lastPollMs = millis();
  LOG_INF("GYR", "SDK IMU initialized");
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
#if FREEINK_DEVICE_READPICO
  _lastAccelMs = 0;
#endif
  return true;
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!_available) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if ((mode != CrossPointTiltPageTurn::TILT_OFF) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF) && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled, skip the rest of the polling logic and avoid unnecessary I2C traffic in non-reader activities
  if ((mode == CrossPointTiltPageTurn::TILT_OFF) || !inReader) {
#if FREEINK_DEVICE_READPICO
    _lastAccelMs = 0;
#endif
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return;
  float gx = sample.gx, gy = sample.gy;
#if FREEINK_DEVICE_READPICO
  if (BoardConfig::isReadPico()) {
    const auto elapsed = now - _lastAccelMs;
    // Near neutral, a change of one g per second approximates one radian per second.
    constexpr float DEG_PER_RADIAN_MS = 57295.78f;
    const bool consecutive = _lastAccelMs != 0 && elapsed > 0 && elapsed <= 3 * POLL_INTERVAL_MS;
    if (consecutive) {
      gx = (sample.ax - _lastAccelX) * -DEG_PER_RADIAN_MS / elapsed;
      gy = (sample.ay - _lastAccelY) * -DEG_PER_RADIAN_MS / elapsed;
    }
    _lastAccelX = sample.ax;
    _lastAccelY = sample.ay;
    _lastAccelMs = now;
    if (!consecutive) return;
  }
#endif

  // Map the gyro axis to left/right tilt based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gx : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gx : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gy : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gy : gy;
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
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
