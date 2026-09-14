#pragma once

#include "HalTiltSensor.h"

class HalTiltSensor::IMUTiltEstimator {
 public:
  enum State : uint8_t { CALIBRATING = 0, NEEDS_TARE = 1, TRACKING = 2 };
  enum TareBlocker : uint8_t {
    GYRO_X = 1,
    GYRO_Y = 2,
    GYRO_Z = 4,
    ACCEL_MAGNITUDE = 8,
    INVALID_SAMPLE = 16,
    ACCEL_DIRECTION = 32,
    GYRO_VARIATION = 64,
    SAMPLE_GAP = 128
  };

 private:
  State stateValue = CALIBRATING;

  // q maps the current sensor/body frame into the frame captured at tare.
  float q[4] = {1, 0, 0, 0};
  float referenceAccel[3] = {0, 0, 1};
  float gyroBias[3] = {};
  float pitchValue = 0, rollValue = 0, twistValue = 0;

  float stationaryAccelMean[3] = {};
  float stationaryGyroMean[3] = {};
  float stationaryAccelAnchor[3] = {};
  uint32_t stationaryStartMs = 0;
  uint16_t stationarySampleCount = 0;
  bool stationaryCandidateForReposition = false;

  uint32_t lastUpdateMs = 0, lastMoveXMs = 0, lastMoveYMs = 0;
  uint32_t lastSampleMs = 0, sampleInterval = 0, integrationInterval = 0;
  uint32_t lastTraceMs = 0, lastTareWaitLogMs = 0;
  uint8_t tareBlockerMask = 0, lastLoggedTareBlockers = 0;
  bool haveSample = false, haveBias = false, referencePendingFromCachedBias = false, pointerOutputReady = false;
  bool loggedInvalidInput = false, loggedInvalidState = false;

  static constexpr float ACCEL_MIN_G = 0.8f;  // Lower acceleration limit for gravity-based correction.
  static constexpr float ACCEL_MAX_G = 1.2f;  // Upper acceleration limit for gravity-based correction.
  static constexpr float STATIONARY_ACCEL_DOT_MIN =
      0.99939f;  // Maximum direction drift during a stable window (approx. 2 deg)
  static constexpr float STATIONARY_GYRO_VARIATION_DPS = 4.0f;  // Maximum sample deviation from the window mean.
  static constexpr float STATIONARY_CORRECTED_RATE_DPS = 2.5f;  // Maximum corrected rate when recovering a reference.
  static constexpr float CALIBRATION_MAX_BIAS_DPS = 20.0f;      // Largest zero-rate offset accepted at startup.
  static constexpr uint32_t CALIBRATION_HOLD_MS = 250;          // Quiet startup time needed to estimate gyro bias.
  static constexpr uint16_t CALIBRATION_MIN_SAMPLES = 10;       // Minimum startup samples used to estimate gyro bias.
  static constexpr uint32_t STATIONARY_HOLD_MS = 500;           // Stable time required to capture a reference.
  static constexpr uint16_t STATIONARY_MIN_SAMPLES = 12;        // Minimum samples required for a stable window.

  static constexpr float REPOSITION_MIN_ANGLE_DEG = 32.0f;  // Relative angle that enables automatic re-tare.
  static constexpr float REPOSITION_MAX_RATE_DPS = 1.75f;   // Maximum mean rate while settling at a new posture.
  static constexpr uint32_t REPOSITION_HOLD_MS = 250;       // Settled time required for automatic re-tare.
  static constexpr uint16_t REPOSITION_MIN_SAMPLES = 8;     // Minimum samples required for automatic re-tare.

  static constexpr float BIAS_ADAPT_MAX_RATE_DPS = 1.75f;  // Maximum residual accepted as stationary drift.
  static constexpr float BIAS_ADAPT_ACCEL_DOT_MIN =
      0.99966f;                                           // Maximum gravity drift during adaptation (approx. 1.5 deg).
  static constexpr float BIAS_ADAPT_ALPHA = 0.15f;        // Fraction of residual bias learned per stable window.
  static constexpr float BIAS_ADAPT_MAX_STEP_DPS = 0.15f; // Maximum per-axis change from one stable window.
  static constexpr uint32_t BIAS_ADAPT_HOLD_MS = 1000;    // Stable time required before updating bias.
  static constexpr uint16_t BIAS_ADAPT_MIN_SAMPLES = 24;  // Minimum samples required before updating bias.

  static constexpr float ATTITUDE_KP = 4.0f;                    // Strength of accelerometer gravity correction.
  static constexpr uint32_t MAX_INTEGRATION_INTERVAL_MS = 50;   // Maximum interval integrated from one sample.
  static constexpr uint32_t SAMPLE_GAP_LOG_THRESHOLD_MS = 100;  // Gap above which integration is skipped.
  static constexpr uint32_t REFERENCE_LOST_INTERVAL_MS = 250;   // Gap above which the reference is reacquired.
  static constexpr uint32_t TARE_WAIT_LOG_INTERVAL_MS = 1000;   // Repeat interval for stable-window diagnostics.

  static constexpr float TRACK_MIN_ANGLE = 3.5f;  // Base angle required to trigger pointer movement.
  static constexpr float TRACK_REPEAT_MAX_ANGLE_F =
      3.25f;  // Multiple of minimum tracking angle that reaches the fastest repeat rate.
  static constexpr float MOVE_REPEAT_MIN_MS = 150.0f;       // Fastest interval between repeated movements.
  static constexpr float SENSITIVITY_FACTOR_LOW = 2.25f;    // Low-sensitivity angle multiplier.
  static constexpr float SENSITIVITY_FACTOR_NORMAL = 1.5f;  // Normal-sensitivity angle multiplier.
  static constexpr float SENSITIVITY_FACTOR_HIGH = 1.0f;    // High-sensitivity angle multiplier.

  // Return the diagnostic label for an estimator state.
  static const char* stateName(State state);

  // Change state and log the transition reason.
  void transitionTo(State state, const char* reason);

  // Reset relative orientation and pointer-repeat state.
  void resetAttitude();

  // Discard the current stationary-window samples.
  void resetStationaryCandidate();

  // Accumulate stable samples until the requested hold completes.
  bool updateStationaryCandidate(float ax, float ay, float az, float gx, float gy, float gz, float rateLimit,
                                 uint32_t holdMs, uint16_t minSamples, bool logRejection = true,
                                 float accelDotMin = STATIONARY_ACCEL_DOT_MIN);

  // Gently move gyro bias toward a stationary window without changing neutral orientation.
  void adaptGyroBias();

  // Remove rotation around gravity while preserving observable tilt from the original neutral.
  void alignAttitudeToStationaryGravity();

  // Capture gravity and gyro bias from the stationary window.
  bool captureReference();

  // Capture only the current gravity reference while preserving a previously learned bias.
  bool captureReferenceFromSample(float ax, float ay, float az);

  // Integrate one corrected sample into the attitude quaternion.
  bool updateAttitude(float ax, float ay, float az, float gx, float gy, float gz, uint32_t dtMs);

  // Convert the attitude to its shortest rotation vector.
  void updateRotationVector();

  // Rate-limit diagnostics for rejected stationary samples.
  void logTareWait(float accelLength, float gxCorrected, float gyCorrected, float gzCorrected);

  // Report the latest sample and estimator state.
  void logDiagnostics(bool firstSample, float ax, float ay, float az, float gx, float gy, float gz);

  // Apply threshold and repeat timing for one pointer axis.
  void getPointerMove(float angle, int& dir, uint32_t& lastMillis, bool invert, float sensitivityFactor);

 public:
  float pitch() const { return pitchValue; }
  float roll() const { return rollValue; }
  float twist() const {
    return twistValue;
  }  // not "yaw", because it's just the axial angle around z in the reference frame, not an absolute yaw direction.

  // Return the norm of the attitude quaternion.
  float attitudeNorm() const;
  float estimatedGyroBias(uint8_t axis) const { return axis < 3 ? gyroBias[axis] : 0; }
  bool hasGyroBias() const { return haveBias; }

  // Read-only observations of the last consumed sample. These do not consume pointer events.
  State state() const { return stateValue; }
  uint8_t tareBlockers() const { return tareBlockerMask; }
  uint32_t sampleIntervalMs() const { return sampleInterval; }
  uint32_t integrationIntervalMs() const { return integrationInterval; }

  // Validate all persistent floating-point estimator state.
  bool hasFiniteState() const;

  // Start a pointer session, optionally reusing a bias learned earlier in this boot.
  void begin(const float* initialGyroBias = nullptr);

  // Consume one acceleration (g), gyro (degrees/second), and millisecond timestamp sample.
  void consume(float ax, float ay, float az, float gx, float gy, float gz, unsigned long timestamp);

  // Poll whether a horizontal or vertical pointer event shall be generated. Sensitivity is 0-2 (low-normal-high). Will
  // update internal state, e.g. the move events are consumed.
  void pollPointerMove(int& moveX, int& moveY, uint8_t orientation, uint8_t sensitivity, bool invertX, bool invertY);
};
