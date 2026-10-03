#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (_available) {
    _initMs = millis();
    _lastPollMs = millis();
    // begin() leaves the sensors sampling; stand them by until tilt page turn
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
    _pose[0] = _pose[1] = _pose[2] = _flickDir = 0;
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
  if (!_sdkImu.read(sample)) {
    return;
  }
  const float gx = sample.gx, gy = sample.gy, g[3] = {sample.ax, sample.ay, sample.az};

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

  // A flick turns the page once at rest near its pose; a pick-up stays turned, and lifting adds to gravity.
  if (_flickDir) {
    float nowSq = 0, offSq = 0;
    for (int i = 0; i < 3; ++i) {
      nowSq += g[i] * g[i];
      offSq += (g[i] - _pose[i]) * (g[i] - _pose[i]);
    }
    if (now - _lastTiltMs > FLICK_WAIT_MS) {
      _flickDir = 0;
      LOG_INF("GYR", "Tilt dropped: device picked up");
    } else if (fabsf(tiltAxis) < CALM_RATE_DPS && fabsf(sqrtf(nowSq) - 1) < REST_G && offSq < POSE_G * POSE_G) {
      (_flickDir > 0 ? _tiltForwardEvent : _tiltBackEvent) = true;
      _flickDir = 0;
    }
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
        if (!_flickDir) _flickDir = 1;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        if (!_flickDir) _flickDir = -1;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }

  if (!_flickDir) {
    for (int i = 0; i < 3; ++i) _pose[i] += (g[i] - _pose[i]) / 8;
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
