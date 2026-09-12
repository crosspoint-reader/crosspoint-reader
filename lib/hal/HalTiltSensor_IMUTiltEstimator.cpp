#include "HalTiltSensor_IMUTiltEstimator.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

#include "HalTiltSensor.h"

#ifndef TILT_POINTER_TRACE_INTERVAL_MS
#define TILT_POINTER_TRACE_INTERVAL_MS 250
#endif

#define _IMU_LOG_NAME_ ("GYR-IMU")

static_assert(TILT_POINTER_TRACE_INTERVAL_MS >= 0);

namespace {

constexpr float DEGREES_TO_RADIANS = std::numbers::pi_v<float> / 180.0f;
constexpr float RADIANS_TO_DEGREES = 180.0f * std::numbers::inv_pi_v<float>;
constexpr float MIN_VECTOR_NORM = 1.0e-6f;

bool finiteSample(float ax, float ay, float az, float gx, float gy, float gz) {
  return std::isfinite(ax) && std::isfinite(ay) && std::isfinite(az) && std::isfinite(gx) && std::isfinite(gy) &&
         std::isfinite(gz);
}

float vectorLength(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }

void rotateVector(float qw, float qx, float qy, float qz, float vx, float vy, float vz, float& rx, float& ry,
                  float& rz) {
  const float tx = 2.0f * (qy * vz - qz * vy);
  const float ty = 2.0f * (qz * vx - qx * vz);
  const float tz = 2.0f * (qx * vy - qy * vx);
  rx = vx + qw * tx + (qy * tz - qz * ty);
  ry = vy + qw * ty + (qz * tx - qx * tz);
  rz = vz + qw * tz + (qx * ty - qy * tx);
}

}  // namespace

void HalTiltSensor::IMUTiltEstimator::begin(const float* initialGyroBias) {
  const bool validInitialBias = initialGyroBias && std::isfinite(initialGyroBias[0]) &&
                                std::isfinite(initialGyroBias[1]) && std::isfinite(initialGyroBias[2]);
  stateValue = validInitialBias ? NEEDS_TARE : CALIBRATING;
  std::fill(std::begin(gyroBias), std::end(gyroBias), 0.0f);
  if (validInitialBias) std::copy_n(initialGyroBias, 3, gyroBias);
  haveBias = validInitialBias;
  referencePendingFromCachedBias = validInitialBias;
  haveSample = false;
  pointerOutputReady = false;
  loggedInvalidInput = false;
  loggedInvalidState = false;
  lastUpdateMs = lastMoveXMs = lastMoveYMs = 0;
  lastSampleMs = sampleInterval = integrationInterval = 0;
  lastTraceMs = lastTareWaitLogMs = 0;
  tareBlockerMask = lastLoggedTareBlockers = 0;
  resetAttitude();
  resetStationaryCandidate();
  LOG_INF(_IMU_LOG_NAME_, "state=%s reason=%s", stateName(stateValue),
          validInitialBias ? "begin_cached_bias" : "begin");
}

const char* HalTiltSensor::IMUTiltEstimator::stateName(const State state) {
  switch (state) {
    case CALIBRATING:
      return "CALIBRATING";
    case NEEDS_TARE:
      return "NEEDS_TARE";
    case TRACKING:
      return "TRACKING";
  }
  return "UNKNOWN";
}

void HalTiltSensor::IMUTiltEstimator::transitionTo(const State state, [[maybe_unused]] const char* reason) {
  LOG_INF(_IMU_LOG_NAME_, "t=%lu state=%s->%s reason=%s bias_x:%.3f bias_y:%.3f bias_z:%.3f",
          static_cast<unsigned long>(lastSampleMs), stateName(stateValue), stateName(state), reason, gyroBias[0],
          gyroBias[1], gyroBias[2]);
  stateValue = state;
  tareBlockerMask = 0;
}

void HalTiltSensor::IMUTiltEstimator::resetAttitude() {
  q[0] = 1.0f;
  q[1] = q[2] = q[3] = 0.0f;
  rollValue = pitchValue = twistValue = 0.0f;
  lastUpdateMs = lastMoveXMs = lastMoveYMs = 0;
  pointerOutputReady = false;
}

void HalTiltSensor::IMUTiltEstimator::resetStationaryCandidate() {
  stationaryStartMs = 0;
  stationarySampleCount = 0;
  std::fill(std::begin(stationaryAccelMean), std::end(stationaryAccelMean), 0.0f);
  std::fill(std::begin(stationaryGyroMean), std::end(stationaryGyroMean), 0.0f);
  std::fill(std::begin(stationaryAccelAnchor), std::end(stationaryAccelAnchor), 0.0f);
}

void HalTiltSensor::IMUTiltEstimator::logTareWait([[maybe_unused]] float accelLength,
                                                  [[maybe_unused]] float gxCorrected,
                                                  [[maybe_unused]] float gyCorrected,
                                                  [[maybe_unused]] float gzCorrected) {
  if (tareBlockerMask == lastLoggedTareBlockers && lastSampleMs - lastTareWaitLogMs < TARE_WAIT_LOG_INTERVAL_MS) return;
  lastLoggedTareBlockers = tareBlockerMask;
  lastTareWaitLogMs = lastSampleMs;
  LOG_INF(_IMU_LOG_NAME_, "t=%lu stationary_wait blocked=0x%02x acc_len:%.3f gyro_corrected:%.3f,%.3f,%.3f",
          static_cast<unsigned long>(lastSampleMs), static_cast<unsigned int>(tareBlockerMask), accelLength,
          gxCorrected, gyCorrected, gzCorrected);
}

bool HalTiltSensor::IMUTiltEstimator::updateStationaryCandidate(float ax, float ay, float az, float gx, float gy,
                                                                float gz, const float rateLimit, const uint32_t holdMs,
                                                                const uint16_t minSamples) {
  const float accelLength = vectorLength(ax, ay, az);
  const float gxCorrected = haveBias ? gx - gyroBias[0] : gx;
  const float gyCorrected = haveBias ? gy - gyroBias[1] : gy;
  const float gzCorrected = haveBias ? gz - gyroBias[2] : gz;

  uint8_t blockers = 0;
  if (accelLength < ACCEL_MIN_G || accelLength > ACCEL_MAX_G || accelLength < MIN_VECTOR_NORM) {
    blockers |= ACCEL_MAGNITUDE;
  }
  if (!haveBias) {
    if (fabsf(gxCorrected) > rateLimit) blockers |= GYRO_X;
    if (fabsf(gyCorrected) > rateLimit) blockers |= GYRO_Y;
    if (fabsf(gzCorrected) > rateLimit) blockers |= GYRO_Z;
  }

  float nx = 0, ny = 0, nz = 0;
  if (!(blockers & ACCEL_MAGNITUDE)) {
    nx = ax / accelLength;
    ny = ay / accelLength;
    nz = az / accelLength;
  }

  if (!blockers && stationarySampleCount > 0) {
    const float accelDot =
        nx * stationaryAccelAnchor[0] + ny * stationaryAccelAnchor[1] + nz * stationaryAccelAnchor[2];
    if (accelDot < STATIONARY_ACCEL_DOT_MIN) blockers |= ACCEL_DIRECTION;
    if (fabsf(gx - stationaryGyroMean[0]) > STATIONARY_GYRO_VARIATION_DPS ||
        fabsf(gy - stationaryGyroMean[1]) > STATIONARY_GYRO_VARIATION_DPS ||
        fabsf(gz - stationaryGyroMean[2]) > STATIONARY_GYRO_VARIATION_DPS) {
      blockers |= GYRO_VARIATION;
    }
  }

  tareBlockerMask = blockers;
  if (blockers) {
    logTareWait(accelLength, gxCorrected, gyCorrected, gzCorrected);
    resetStationaryCandidate();
    return false;
  }

  if (stationarySampleCount == 0) {
    stationaryStartMs = lastSampleMs;
    stationarySampleCount = 1;
    stationaryAccelMean[0] = ax;
    stationaryAccelMean[1] = ay;
    stationaryAccelMean[2] = az;
    stationaryAccelAnchor[0] = nx;
    stationaryAccelAnchor[1] = ny;
    stationaryAccelAnchor[2] = nz;
    stationaryGyroMean[0] = gx;
    stationaryGyroMean[1] = gy;
    stationaryGyroMean[2] = gz;
    return false;
  }

  ++stationarySampleCount;
  const float alpha = 1.0f / stationarySampleCount;
  for (unsigned int axis = 0; axis < 3; ++axis) {
    const float accel = axis == 0 ? ax : axis == 1 ? ay : az;
    const float gyro = axis == 0 ? gx : axis == 1 ? gy : gz;
    stationaryAccelMean[axis] += (accel - stationaryAccelMean[axis]) * alpha;
    stationaryGyroMean[axis] += (gyro - stationaryGyroMean[axis]) * alpha;
  }

  if (stationarySampleCount < minSamples || lastSampleMs - stationaryStartMs < holdMs) return false;

  if (haveBias) {
    const float meanGxCorrected = stationaryGyroMean[0] - gyroBias[0];
    const float meanGyCorrected = stationaryGyroMean[1] - gyroBias[1];
    const float meanGzCorrected = stationaryGyroMean[2] - gyroBias[2];
    if (fabsf(meanGxCorrected) > rateLimit) blockers |= GYRO_X;
    if (fabsf(meanGyCorrected) > rateLimit) blockers |= GYRO_Y;
    if (fabsf(meanGzCorrected) > rateLimit) blockers |= GYRO_Z;
    tareBlockerMask = blockers;
    if (blockers) {
      logTareWait(accelLength, meanGxCorrected, meanGyCorrected, meanGzCorrected);
      resetStationaryCandidate();
      return false;
    }
  }

  return true;
}

bool HalTiltSensor::IMUTiltEstimator::captureReference() {
  const float accelLength = vectorLength(stationaryAccelMean[0], stationaryAccelMean[1], stationaryAccelMean[2]);
  if (!std::isfinite(accelLength) || accelLength < MIN_VECTOR_NORM) return false;

  for (unsigned int axis = 0; axis < 3; ++axis) {
    referenceAccel[axis] = stationaryAccelMean[axis] / accelLength;
    gyroBias[axis] = stationaryGyroMean[axis];
  }
  haveBias = true;
  LOG_INF(_IMU_LOG_NAME_, "tare t=%lu gravity:%.3f,%.3f,%.3f bias:%.3f,%.3f,%.3f samples=%u",
          static_cast<unsigned long>(lastSampleMs), referenceAccel[0], referenceAccel[1], referenceAccel[2],
          gyroBias[0], gyroBias[1], gyroBias[2], static_cast<unsigned int>(stationarySampleCount));
  resetAttitude();
  return true;
}

bool HalTiltSensor::IMUTiltEstimator::captureReferenceFromSample(float ax, float ay, float az) {
  const float accelLength = vectorLength(ax, ay, az);
  if (!std::isfinite(accelLength) || accelLength < ACCEL_MIN_G || accelLength > ACCEL_MAX_G) return false;

  referenceAccel[0] = ax / accelLength;
  referenceAccel[1] = ay / accelLength;
  referenceAccel[2] = az / accelLength;
  LOG_INF(_IMU_LOG_NAME_, "tare t=%lu gravity:%.3f,%.3f,%.3f bias:%.3f,%.3f,%.3f samples=1 source=cached_bias",
          static_cast<unsigned long>(lastSampleMs), referenceAccel[0], referenceAccel[1], referenceAccel[2],
          gyroBias[0], gyroBias[1], gyroBias[2]);
  resetAttitude();
  return true;
}

bool HalTiltSensor::IMUTiltEstimator::updateAttitude(float ax, float ay, float az, float gx, float gy, float gz,
                                                     uint32_t dtMs) {
  integrationInterval = std::min(dtMs, MAX_INTEGRATION_INTERVAL_MS);
  if (integrationInterval == 0) return true;

  float errorX = 0, errorY = 0, errorZ = 0;
  const float accelLength = vectorLength(ax, ay, az);
  if (accelLength >= ACCEL_MIN_G && accelLength <= ACCEL_MAX_G) {
    const float measuredX = ax / accelLength;
    const float measuredY = ay / accelLength;
    const float measuredZ = az / accelLength;
    float predictedX, predictedY, predictedZ;
    rotateVector(q[0], -q[1], -q[2], -q[3], referenceAccel[0], referenceAccel[1], referenceAccel[2], predictedX,
                 predictedY, predictedZ);
    errorX = measuredY * predictedZ - measuredZ * predictedY;
    errorY = measuredZ * predictedX - measuredX * predictedZ;
    errorZ = measuredX * predictedY - measuredY * predictedX;
  }

  const float omegaX = (gx - gyroBias[0]) * DEGREES_TO_RADIANS + ATTITUDE_KP * errorX;
  const float omegaY = (gy - gyroBias[1]) * DEGREES_TO_RADIANS + ATTITUDE_KP * errorY;
  const float omegaZ = (gz - gyroBias[2]) * DEGREES_TO_RADIANS + ATTITUDE_KP * errorZ;
  const float dt = integrationInterval * 0.001f;
  const float phiX = omegaX * dt;
  const float phiY = omegaY * dt;
  const float phiZ = omegaZ * dt;
  const float angle = vectorLength(phiX, phiY, phiZ);

  float dw, dx, dy, dz;
  if (angle > MIN_VECTOR_NORM) {
    const float halfAngle = 0.5f * angle;
    const float scale = sinf(halfAngle) / angle;
    dw = cosf(halfAngle);
    dx = phiX * scale;
    dy = phiY * scale;
    dz = phiZ * scale;
  } else {
    dw = 1.0f;
    dx = 0.5f * phiX;
    dy = 0.5f * phiY;
    dz = 0.5f * phiZ;
  }

  const float nw = q[0] * dw - q[1] * dx - q[2] * dy - q[3] * dz;
  const float nx = q[0] * dx + q[1] * dw + q[2] * dz - q[3] * dy;
  const float ny = q[0] * dy - q[1] * dz + q[2] * dw + q[3] * dx;
  const float nz = q[0] * dz + q[1] * dy - q[2] * dx + q[3] * dw;
  const float norm = sqrtf(nw * nw + nx * nx + ny * ny + nz * nz);
  
  if (!std::isfinite(norm) || norm < MIN_VECTOR_NORM) return false;

  q[0] = nw / norm;
  q[1] = nx / norm;
  q[2] = ny / norm;
  q[3] = nz / norm;

  updateRotationVector();
  
  return hasFiniteState();
}

void HalTiltSensor::IMUTiltEstimator::updateRotationVector() {
  const float hemisphere = q[0] < 0 ? -1.0f : 1.0f;
  const float qw = q[0] * hemisphere;
  const float qx = q[1] * hemisphere;
  const float qy = q[2] * hemisphere;
  const float qz = q[3] * hemisphere;
  const float vectorNorm = vectorLength(qx, qy, qz);
  const float scale = vectorNorm > MIN_VECTOR_NORM ? 2.0f * atan2f(vectorNorm, qw) * RADIANS_TO_DEGREES / vectorNorm
                                                   : 2.0f * RADIANS_TO_DEGREES;
  rollValue = qx * scale;
  pitchValue = qy * scale;
  twistValue = qz * scale;
}

float HalTiltSensor::IMUTiltEstimator::attitudeNorm() const {
  return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
}

bool HalTiltSensor::IMUTiltEstimator::hasFiniteState() const {
  if (!std::all_of(std::begin(q), std::end(q), [](const float value) { return std::isfinite(value); })) return false;
  if (!std::all_of(std::begin(referenceAccel), std::end(referenceAccel),
                   [](const float value) { return std::isfinite(value); })) {
    return false;
  }
  if (!std::all_of(std::begin(gyroBias), std::end(gyroBias), [](const float value) { return std::isfinite(value); })) {
    return false;
  }
  const float norm = attitudeNorm();
  return std::isfinite(rollValue) && std::isfinite(pitchValue) && std::isfinite(twistValue) && norm > 0.999f &&
         norm < 1.001f;
}

void HalTiltSensor::IMUTiltEstimator::logDiagnostics([[maybe_unused]] bool firstSample, [[maybe_unused]] float ax,
                                                     [[maybe_unused]] float ay, [[maybe_unused]] float az,
                                                     [[maybe_unused]] float gx, [[maybe_unused]] float gy,
                                                     [[maybe_unused]] float gz) {
#ifdef ENABLE_SERIAL_LOG
  const bool finite = hasFiniteState();
  if (!finite && !loggedInvalidState) {
    LOG_ERR(_IMU_LOG_NAME_, "t=%lu state=%s fault=nonfinite_state", static_cast<unsigned long>(lastSampleMs),
            stateName(stateValue));
  } else if (finite && loggedInvalidState) {
    LOG_INF(_IMU_LOG_NAME_, "t=%lu state=%s reason=finite_state_restored", static_cast<unsigned long>(lastSampleMs),
            stateName(stateValue));
  }
  loggedInvalidState = !finite;

  if (sampleInterval > SAMPLE_GAP_LOG_THRESHOLD_MS) {
    LOG_INF(_IMU_LOG_NAME_, "t=%lu sample_gap dt_ms=%lu action=%s", static_cast<unsigned long>(lastSampleMs),
            static_cast<unsigned long>(sampleInterval),
            sampleInterval > REFERENCE_LOST_INTERVAL_MS ? "reacquire" : "skip");
  }

#if LOG_LEVEL >= 2
  if constexpr (TILT_POINTER_TRACE_INTERVAL_MS > 0) {
    if (!firstSample && lastSampleMs - lastTraceMs < TILT_POINTER_TRACE_INTERVAL_MS) return;
  }
  lastTraceMs = lastSampleMs;
  LOG_DBG(_IMU_LOG_NAME_, "sample t=%lu dt_ms=%lu used_ms=%lu acc:%.3f,%.3f,%.3f gyro:%.3f,%.3f,%.3f",
          static_cast<unsigned long>(lastSampleMs), static_cast<unsigned long>(sampleInterval),
          static_cast<unsigned long>(integrationInterval), ax, ay, az, gx, gy, gz);
  LOG_DBG(_IMU_LOG_NAME_, "estimate t=%lu state=%s blocked=0x%02x finite=%d bias:%.3f,%.3f,%.3f",
          static_cast<unsigned long>(lastSampleMs), stateName(stateValue), static_cast<unsigned int>(tareBlockerMask),
          finite, gyroBias[0], gyroBias[1], gyroBias[2]);
  LOG_DBG(_IMU_LOG_NAME_, "attitude t=%lu q:%.4f,%.4f,%.4f,%.4f rotation:%.3f,%.3f,%.3f",
          static_cast<unsigned long>(lastSampleMs), q[0], q[1], q[2], q[3], rollValue, pitchValue, twistValue);
#endif
#endif
}

void HalTiltSensor::IMUTiltEstimator::consume(float ax, float ay, float az, float gx, float gy, float gz,
                                              unsigned long timestamp) {
  const bool firstSample = !haveSample;
  const auto sampleMs = static_cast<uint32_t>(timestamp);
  sampleInterval = firstSample ? 0 : sampleMs - lastSampleMs;
  lastSampleMs = sampleMs;
  haveSample = true;
  integrationInterval = 0;

  if (!finiteSample(ax, ay, az, gx, gy, gz)) {
    tareBlockerMask = INVALID_SAMPLE;
    pointerOutputReady = false;
    resetStationaryCandidate();
    if (!loggedInvalidInput) {
      LOG_ERR(_IMU_LOG_NAME_, "t=%lu state=%s fault=invalid_sample", static_cast<unsigned long>(lastSampleMs),
              stateName(stateValue));
      loggedInvalidInput = true;
    }
    logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
    return;
  }
  if (loggedInvalidInput) {
    LOG_INF(_IMU_LOG_NAME_, "t=%lu state=%s reason=valid_samples_restored", static_cast<unsigned long>(lastSampleMs),
            stateName(stateValue));
    loggedInvalidInput = false;
  }

  if (!firstSample && sampleInterval > REFERENCE_LOST_INTERVAL_MS) {
    pointerOutputReady = false;
    tareBlockerMask = SAMPLE_GAP;
    resetStationaryCandidate();
    resetAttitude();
    if (stateValue == TRACKING) transitionTo(NEEDS_TARE, "sample_gap");
    logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
    return;
  }

  if (stateValue != TRACKING) {
    if (referencePendingFromCachedBias && captureReferenceFromSample(ax, ay, az)) {
      referencePendingFromCachedBias = false;
      transitionTo(TRACKING, "cached_bias_tare");
      resetStationaryCandidate();
      logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
      return;
    }

    const float rateLimit = haveBias ? STATIONARY_CORRECTED_RATE_DPS : CALIBRATION_MAX_BIAS_DPS;
    const uint32_t holdMs = haveBias ? STATIONARY_HOLD_MS : CALIBRATION_HOLD_MS;
    const uint16_t minSamples = haveBias ? STATIONARY_MIN_SAMPLES : CALIBRATION_MIN_SAMPLES;
    if (updateStationaryCandidate(ax, ay, az, gx, gy, gz, rateLimit, holdMs, minSamples)) {
      if (stateValue == CALIBRATING) transitionTo(NEEDS_TARE, "stationary_bias_ready");
      if (captureReference()) transitionTo(TRACKING, "tare_accepted");
      referencePendingFromCachedBias = false;
      resetStationaryCandidate();
    }
    logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
    return;
  }

  if (firstSample || sampleInterval == 0 || sampleInterval > SAMPLE_GAP_LOG_THRESHOLD_MS) {
    pointerOutputReady = false;
    logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
    return;
  }

  if (!updateAttitude(ax, ay, az, gx, gy, gz, sampleInterval)) {
    LOG_ERR(_IMU_LOG_NAME_, "t=%lu state=%s fault=attitude_update", static_cast<unsigned long>(lastSampleMs),
            stateName(stateValue));
    resetAttitude();
    resetStationaryCandidate();
    transitionTo(NEEDS_TARE, "attitude_update_failed");
  } else {
    lastUpdateMs = lastSampleMs;
    pointerOutputReady = true;
    const float relativeAngle = vectorLength(rollValue, pitchValue, twistValue);
    if (relativeAngle >= REPOSITION_MIN_ANGLE_DEG) {
      pointerOutputReady = false;
      if (updateStationaryCandidate(ax, ay, az, gx, gy, gz, REPOSITION_MAX_RATE_DPS, REPOSITION_HOLD_MS,
                                    REPOSITION_MIN_SAMPLES)) {
        transitionTo(NEEDS_TARE, "posture_change");
        if (captureReference()) transitionTo(TRACKING, "tare_accepted");
        resetStationaryCandidate();
      }
    } else {
      tareBlockerMask = 0;
      resetStationaryCandidate();
    }
  }
  logDiagnostics(firstSample, ax, ay, az, gx, gy, gz);
}

void HalTiltSensor::IMUTiltEstimator::getPointerMove(float angle, int& dir, uint32_t& lastMillis, bool invert,
                                                     const float sensitivityFactor) {
  const float absAngle = fabsf(angle);
  const float minAngle = TRACK_MIN_ANGLE * sensitivityFactor;
  const int movement = absAngle >= minAngle ? (angle > 0 ? -1 : 1) * (invert ? -1 : 1) : 0;

  if (movement &&
      (lastMillis == 0 || ((lastMillis & 1UL) != ((movement > 0) ? 0 : 1)) ||
       ((lastUpdateMs - lastMillis) >
        (MOVE_REPEAT_MIN_MS *
         (1.0f + 2.0f * std::max(0.0f, 1.0f - (absAngle - minAngle) / (TRACK_REPEAT_MAX_ANGLE - minAngle))))))) {
    dir = movement;
    lastMillis = ((lastUpdateMs - 2) & ~1UL) | ((movement > 0) ? 0 : 1);
  } else {
    dir = 0;
    if (movement == 0) lastMillis = 0;
  }
}

void HalTiltSensor::IMUTiltEstimator::pollPointerMove(int& moveX, int& moveY, uint8_t orientation, uint8_t sensitivity,
                                                      bool invertX, bool invertY) {
  moveX = moveY = 0;
  if (stateValue != TRACKING || !pointerOutputReady) return;

  float sensitivityFactor = SENSITIVITY_FACTOR_NORMAL;
  if (sensitivity == 0)
    sensitivityFactor = SENSITIVITY_FACTOR_LOW;
  else if (sensitivity == 2)
    sensitivityFactor = SENSITIVITY_FACTOR_HIGH;

  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      getPointerMove(rollValue, moveX, lastMoveXMs, invertX, sensitivityFactor);
      getPointerMove(pitchValue, moveY, lastMoveYMs, invertY, sensitivityFactor);
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      getPointerMove(pitchValue, moveX, lastMoveXMs, invertX, sensitivityFactor);
      getPointerMove(rollValue, moveY, lastMoveYMs, !invertY, sensitivityFactor);
      break;
    case CrossPointOrientation::INVERTED:
      getPointerMove(rollValue, moveX, lastMoveXMs, !invertX, sensitivityFactor);
      getPointerMove(pitchValue, moveY, lastMoveYMs, !invertY, sensitivityFactor);
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      getPointerMove(pitchValue, moveX, lastMoveXMs, !invertX, sensitivityFactor);
      getPointerMove(rollValue, moveY, lastMoveYMs, invertY, sensitivityFactor);
      break;
  }

  if (moveX || moveY) {
    LOG_INF(_IMU_LOG_NAME_, "pointer t=%lu orientation=%u sensitivity=%u move_x=%d move_y=%d rotation:%.3f,%.3f,%.3f",
            static_cast<unsigned long>(lastSampleMs), static_cast<unsigned int>(orientation),
            static_cast<unsigned int>(sensitivity), moveX, moveY, rollValue, pitchValue, twistValue);
  }
}
