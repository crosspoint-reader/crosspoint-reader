#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string_view>

#include "HalTiltSensor_IMUTiltEstimator.h"

namespace {

using Estimator = HalTiltSensor::IMUTiltEstimator;
constexpr float RAD_PER_DEG = std::numbers::pi_v<float> / 180.0f;
constexpr uint32_t SAMPLE_MS = 25;

// Host-only capture, sized for the longest synthetic trace; no firmware logging buffers are added.
std::array<char, 131072> capturedLog{};
size_t capturedLogSize = 0;
bool logTruncated = false;

void appendLog(const char* format, va_list args) {
  const size_t available = capturedLog.size() - capturedLogSize;
  const int length = vsnprintf(capturedLog.data() + capturedLogSize, available, format, args);
  if (length < 0 || static_cast<size_t>(length) >= available) {
    logTruncated = true;
    return;
  }
  capturedLogSize += static_cast<size_t>(length);
}

size_t countLog(const std::string_view needle) {
  const std::string_view log(capturedLog.data(), capturedLogSize);
  size_t count = 0;
  for (size_t pos = 0; (pos = log.find(needle, pos)) != std::string_view::npos; pos += needle.size()) ++count;
  return count;
}

struct Move {
  int x = 0;
  int y = 0;
};

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

struct Quaternion {
  float w = 1;
  float x = 0;
  float y = 0;
  float z = 0;
};

Vec3 normalized(const Vec3 v) {
  const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return {v.x / length, v.y / length, v.z / length};
}

Quaternion multiply(const Quaternion a, const Quaternion b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Vec3 rotate(const Quaternion q, const Vec3 v) {
  const Quaternion result = multiply(multiply(q, {0, v.x, v.y, v.z}), {q.w, -q.x, -q.y, -q.z});
  return {result.x, result.y, result.z};
}

Quaternion deltaQuaternion(const Vec3 rateDps, const uint32_t intervalMs) {
  const float scale = RAD_PER_DEG * intervalMs * 0.001f;
  const Vec3 phi{rateDps.x * scale, rateDps.y * scale, rateDps.z * scale};
  const float angle = std::sqrt(phi.x * phi.x + phi.y * phi.y + phi.z * phi.z);
  if (angle < 1.0e-6f) return {};
  const float vectorScale = std::sin(angle * 0.5f) / angle;
  return {std::cos(angle * 0.5f), phi.x * vectorScale, phi.y * vectorScale, phi.z * vectorScale};
}

Vec3 rotationVector(const Quaternion value) {
  const float hemisphere = value.w < 0 ? -1.0f : 1.0f;
  const Quaternion q{value.w * hemisphere, value.x * hemisphere, value.y * hemisphere, value.z * hemisphere};
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  const float scale = length > 1.0e-6f ? 2.0f * std::atan2(length, q.w) / length / RAD_PER_DEG : 2.0f / RAD_PER_DEG;
  return {q.x * scale, q.y * scale, q.z * scale};
}

struct Trace {
  Estimator estimator;
  uint32_t timestamp = 1000;

  Trace() { estimator.begin(); }

  void sample(float ax, float ay, float az, float gx = 0, float gy = 0, float gz = 0, uint32_t interval = SAMPLE_MS) {
    timestamp += interval;
    estimator.consume(ax, ay, az, gx, gy, gz, timestamp);
  }

  // A rotation about body X: gravity and angular rate describe the same physical motion.
  void roll(float angle, float rate = 0, float biasX = 0, float biasZ = 0, uint32_t interval = SAMPLE_MS) {
    sample(0, std::sin(angle * RAD_PER_DEG), std::cos(angle * RAD_PER_DEG), rate + biasX, 0, biasZ, interval);
  }

  void hold(float angle = 30, uint32_t duration = 3000, float biasX = 0, float biasZ = 0) {
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += SAMPLE_MS) roll(angle, 0, biasX, biasZ);
  }

  Move poll(uint8_t orientation = CrossPointOrientation::PORTRAIT, uint8_t sensitivity = 1, bool invertX = false,
            bool invertY = false) {
    Move move;
    estimator.pollPointerMove(move.x, move.y, orientation, sensitivity, invertX, invertY);
    return move;
  }

  void tiltEightDegrees() {
    for (int step = 1; step <= 40; ++step) roll(30.0f + step * 0.2f, 8);
  }
};

struct AttitudeTrace {
  Estimator estimator;
  Vec3 referenceGravity;
  Vec3 bias;
  Quaternion truth;
  uint32_t timestamp = 1000;

  explicit AttitudeTrace(Vec3 gravity = {0, 0, 1}, Vec3 gyroBias = {})
      : referenceGravity(normalized(gravity)), bias(gyroBias) {
    estimator.begin();
  }

  void step(const Vec3 rateDps = {}, uint32_t intervalMs = SAMPLE_MS, float accelerationScale = 1.0f) {
    timestamp += intervalMs;
    truth = multiply(truth, deltaQuaternion(rateDps, intervalMs));
    const Vec3 acceleration = rotate({truth.w, -truth.x, -truth.y, -truth.z}, referenceGravity);
    estimator.consume(acceleration.x * accelerationScale, acceleration.y * accelerationScale,
                      acceleration.z * accelerationScale, rateDps.x + bias.x, rateDps.y + bias.y, rateDps.z + bias.z,
                      timestamp);
  }

  void hold(uint32_t durationMs = 1500) {
    for (uint32_t elapsed = 0; elapsed < durationMs; elapsed += SAMPLE_MS) step();
  }

  Move poll() {
    Move move;
    estimator.pollPointerMove(move.x, move.y, CrossPointOrientation::PORTRAIT, 1, false, false);
    return move;
  }
};

class IMUTiltEstimatorTest : public testing::Test {
 protected:
  void SetUp() override {
    capturedLogSize = 0;
    capturedLog[0] = '\0';
    logTruncated = false;
  }

  void TearDown() override { EXPECT_FALSE(logTruncated); }
};

TEST_F(IMUTiltEstimatorTest, CalibrationSuppressesPointerEvents) {
  Trace trace;
  for (int i = 0; i < 8; ++i) {
    trace.roll(30, 0, 10);
    const Move move = trace.poll();
    EXPECT_EQ(move.x, 0);
    EXPECT_EQ(move.y, 0);
  }
  EXPECT_EQ(trace.estimator.state(), Estimator::CALIBRATING);
}

TEST_F(IMUTiltEstimatorTest, StationaryXBiasConvergesWithoutCursorDrift) {
  Trace trace;
  trace.hold(30, 5000, 10);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  ASSERT_TRUE(trace.estimator.hasFiniteState());
  for (int i = 0; i < 80; ++i) {
    trace.roll(30, 0, 10);
    const Move move = trace.poll();
    EXPECT_EQ(move.x, 0);
    EXPECT_EQ(move.y, 0);
  }
}

TEST_F(IMUTiltEstimatorTest, MeasuredStationaryNoiseStillCalibrates) {
  static constexpr std::array<Vec3, 20> GYRO_SAMPLES = {
      Vec3{13.969f, 1.234f, 0.078f},  Vec3{12.922f, 0.172f, 0.594f},  Vec3{13.641f, 0.953f, 0.062f},
      Vec3{7.844f, 4.312f, -0.672f},  Vec3{11.797f, 2.406f, -0.078f}, Vec3{14.734f, 0.859f, -0.453f},
      Vec3{12.328f, 1.422f, -0.281f}, Vec3{15.062f, 2.359f, -0.250f}, Vec3{15.609f, 1.156f, -0.422f},
      Vec3{11.078f, 1.906f, -0.562f}, Vec3{14.359f, 1.734f, -0.484f}, Vec3{12.734f, 0.859f, 0.141f},
      Vec3{10.703f, 1.250f, -0.156f}, Vec3{10.938f, 2.016f, 0.125f},  Vec3{17.641f, 1.281f, -0.094f},
      Vec3{12.906f, 1.766f, -0.141f}, Vec3{14.625f, 1.141f, -0.141f}, Vec3{10.984f, 1.672f, 0.125f},
      Vec3{12.922f, 2.609f, -0.641f}, Vec3{13.469f, 1.906f, 0.172f},
  };

  Trace trace;
  for (size_t index = 0; index < GYRO_SAMPLES.size(); ++index) {
    const Vec3 gyro = GYRO_SAMPLES[index];
    trace.sample(-0.275f, -0.063f, -0.908f, gyro.x, gyro.y, gyro.z, 34);
    if (index == 7) EXPECT_EQ(trace.estimator.state(), Estimator::CALIBRATING);
    if (index == 8) EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  }

  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(0), 13.0f, 0.5f);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(1), 1.65f, 0.2f);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(2), -0.16f, 0.1f);
}

TEST_F(IMUTiltEstimatorTest, CachedBiasTaresFromFirstSampleWhileAlreadyMoving) {
  static constexpr std::array<float, 3> GYRO_BIAS = {13.0f, 1.5f, -0.1f};

  Trace trace;
  trace.estimator.begin(GYRO_BIAS.data());
  trace.roll(30, 40, GYRO_BIAS[0], GYRO_BIAS[2]);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(0), GYRO_BIAS[0], 0.01f);

  for (int step = 1; step <= 40; ++step) {
    trace.roll(30.0f + step * 0.2f, 8, GYRO_BIAS[0], GYRO_BIAS[2]);
  }
  EXPECT_NE(trace.poll().x, 0);
  EXPECT_EQ(countLog("state=NEEDS_TARE reason=begin_cached_bias"), 1U);
  EXPECT_EQ(countLog("state=NEEDS_TARE->TRACKING reason=cached_bias_tare"), 1U);
}

TEST_F(IMUTiltEstimatorTest, HeldTiltRepeatsAndReturningToNeutralStopsMovement) {
  Trace trace;
  trace.hold();
  trace.tiltEightDegrees();
  int events = 0;
  int direction = 0;
  for (int i = 0; i < 80; ++i) {
    trace.roll(38);
    const Move move = trace.poll();
    if (move.x) {
      if (direction) EXPECT_EQ(move.x, direction);
      direction = move.x;
      ++events;
    }
    EXPECT_EQ(move.y, 0);
  }
  EXPECT_GE(events, 3);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);

  for (int step = 1; step <= 40; ++step) trace.roll(38.0f - step * 0.2f, -8);
  trace.hold(30, 1000);
  EXPECT_NEAR(trace.estimator.roll(), 0, 1);
  const Move move = trace.poll();
  EXPECT_EQ(move.x, 0);
  EXPECT_EQ(move.y, 0);
}

TEST_F(IMUTiltEstimatorTest, OrientationAndUserInversionMapPointerAxes) {
  for (uint8_t orientation = 0; orientation < 4; ++orientation) {
    SCOPED_TRACE(orientation);
    Trace trace;
    trace.hold();
    trace.tiltEightDegrees();
    const Move normal = trace.poll(orientation);
    const Move inverted = trace.poll(orientation, 1, true, true);
    if (orientation == CrossPointOrientation::PORTRAIT || orientation == CrossPointOrientation::INVERTED) {
      EXPECT_NE(normal.x, 0);
      EXPECT_EQ(normal.y, 0);
    } else {
      EXPECT_EQ(normal.x, 0);
      EXPECT_NE(normal.y, 0);
    }
    EXPECT_EQ(inverted.x, -normal.x);
    EXPECT_EQ(inverted.y, -normal.y);
  }
}

TEST_F(IMUTiltEstimatorTest, SensitivityChangesActivationThreshold) {
  Trace trace;
  trace.hold();
  for (int step = 1; step <= 40; ++step) trace.roll(30.0f + step * 0.1f, 4);
  trace.hold(34, 500);
  EXPECT_EQ(trace.poll(CrossPointOrientation::PORTRAIT, 0).x, 0);
  EXPECT_NE(trace.poll(CrossPointOrientation::PORTRAIT, 2).x, 0);
}

TEST_F(IMUTiltEstimatorTest, ModestSampleJitterPreservesStationaryTracking) {
  Trace trace;
  trace.hold(30, 5000, 10);
  for (int i = 0; i < 40; ++i) {
    for (const uint32_t interval : {20U, 35U, 40U}) trace.roll(30, 0, 10, 0, interval);
  }
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  EXPECT_NEAR(trace.estimator.roll(), 0, 1);
  EXPECT_EQ(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, AxisAlignedAndArbitraryStartingPositionsTareWithoutSingularities) {
  constexpr std::array<Vec3, 7> gravities = {
      Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}, Vec3{1, 2, -3},
  };
  for (const Vec3 gravity : gravities) {
    SCOPED_TRACE(testing::Message() << gravity.x << ',' << gravity.y << ',' << gravity.z);
    AttitudeTrace trace(gravity, {10, -6, 3});
    trace.hold();
    EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
    EXPECT_TRUE(trace.estimator.hasFiniteState());
    EXPECT_NEAR(trace.estimator.attitudeNorm(), 1, 0.0001f);
    EXPECT_NEAR(trace.estimator.roll(), 0, 0.01f);
    EXPECT_NEAR(trace.estimator.pitch(), 0, 0.01f);
    EXPECT_NEAR(trace.estimator.twist(), 0, 0.01f);
    EXPECT_NEAR(trace.estimator.estimatedGyroBias(0), 10, 0.01f);
    EXPECT_NEAR(trace.estimator.estimatedGyroBias(1), -6, 0.01f);
    EXPECT_NEAR(trace.estimator.estimatedGyroBias(2), 3, 0.01f);
  }
}

TEST_F(IMUTiltEstimatorTest, TracksThreeAxisBodyRatesAgainstQuaternionTruth) {
  constexpr std::array<Vec3, 3> rates = {Vec3{8, 0, 0}, Vec3{0, -8, 0}, Vec3{0, 0, 8}};
  for (const Vec3 rate : rates) {
    SCOPED_TRACE(testing::Message() << rate.x << ',' << rate.y << ',' << rate.z);
    AttitudeTrace trace({1, 2, -3}, {10, -6, 3});
    trace.hold();
    for (int i = 0; i < 40; ++i) trace.step(rate);
    const Vec3 expected = rotationVector(trace.truth);
    EXPECT_NEAR(trace.estimator.roll(), expected.x, 0.2f);
    EXPECT_NEAR(trace.estimator.pitch(), expected.y, 0.2f);
    EXPECT_NEAR(trace.estimator.twist(), expected.z, 0.2f);
    EXPECT_NEAR(trace.estimator.attitudeNorm(), 1, 0.0001f);
  }
}

TEST_F(IMUTiltEstimatorTest, TracksMixedRotationAgainstQuaternionTruth) {
  AttitudeTrace trace({1, -2, -3}, {10, -6, 3});
  trace.hold();
  for (int i = 0; i < 24; ++i) trace.step({8, -5, 3});
  for (int i = 0; i < 20; ++i) trace.step({-2, 7, 4});
  const Vec3 expected = rotationVector(trace.truth);
  EXPECT_NEAR(trace.estimator.roll(), expected.x, 0.3f);
  EXPECT_NEAR(trace.estimator.pitch(), expected.y, 0.3f);
  EXPECT_NEAR(trace.estimator.twist(), expected.z, 0.3f);
}

TEST_F(IMUTiltEstimatorTest, AccelerationOutsideGravityGateDoesNotTiltAttitude) {
  AttitudeTrace trace({1, 2, -3});
  trace.hold();
  for (int i = 0; i < 20; ++i) trace.step({}, SAMPLE_MS, 1.4f);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  EXPECT_NEAR(trace.estimator.roll(), 0, 0.01f);
  EXPECT_NEAR(trace.estimator.pitch(), 0, 0.01f);
  EXPECT_NEAR(trace.estimator.twist(), 0, 0.01f);
}

TEST_F(IMUTiltEstimatorTest, ModerateSampleGapSkipsUnknownMotion) {
  Trace trace;
  trace.hold();
  trace.roll(30, 100, 0, 0, 125);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_EQ(trace.estimator.integrationIntervalMs(), 0U);
  EXPECT_NEAR(trace.estimator.roll(), 0, 0.01f);
  EXPECT_EQ(trace.poll().x, 0);
  EXPECT_EQ(countLog("sample_gap dt_ms=125 action=skip"), 1U);
}

TEST_F(IMUTiltEstimatorTest, LongSampleGapRequiresAStableNewReference) {
  Trace trace;
  trace.hold(30);
  trace.roll(60, 0, 0, 0, 500);
  EXPECT_EQ(trace.estimator.state(), Estimator::NEEDS_TARE);
  EXPECT_EQ(trace.poll().x, 0);
  EXPECT_EQ(countLog("state=TRACKING->NEEDS_TARE reason=sample_gap"), 1U);
  trace.hold(60, 1000);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.roll(), 0, 0.01f);
  EXPECT_EQ(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, IntegrationIntervalIsClamped) {
  Trace trace;
  trace.hold();
  trace.roll(37.5f, 100, 0, 0, 75);
  EXPECT_EQ(trace.estimator.integrationIntervalMs(), 50U);
  EXPECT_GT(trace.estimator.roll(), 5);
  EXPECT_LT(trace.estimator.roll(), 7.5f);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
}

TEST_F(IMUTiltEstimatorTest, MillisecondWrapPreservesCalibrationAndTracking) {
  Trace trace;
  trace.timestamp = std::numeric_limits<uint32_t>::max() - 400;
  trace.hold(30, 1500, 10);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  for (int step = 1; step <= 40; ++step) trace.roll(30.0f + step * 0.2f, 8, 10);
  EXPECT_NEAR(trace.estimator.roll(), 8, 1);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
}

TEST_F(IMUTiltEstimatorTest, FaceUpAndFaceDownUseTheSameRelativeDirection) {
  int direction = 0;
  for (const float startAngle : {0.0f, 180.0f}) {
    Trace trace;
    trace.hold(startAngle);
    for (int step = 1; step <= 40; ++step) trace.roll(startAngle + step * 0.2f, 8);
    const Move move = trace.poll();
    ASSERT_NE(move.x, 0);
    if (direction) EXPECT_EQ(move.x, direction);
    direction = move.x;
  }
}

TEST_F(IMUTiltEstimatorTest, ReportsStateTransitionsWithReasons) {
  Trace trace;
  trace.hold();
  EXPECT_EQ(countLog("state=CALIBRATING reason=begin"), 1U);
  EXPECT_EQ(countLog("state=CALIBRATING->NEEDS_TARE reason=stationary_bias_ready"), 1U);
  EXPECT_EQ(countLog("state=NEEDS_TARE->TRACKING reason=tare_accepted"), 1U);
}

TEST_F(IMUTiltEstimatorTest, ReportsAllTareBlockersAndThrottlesWaitLogs) {
  Trace trace;
  for (int i = 0; i < 80; ++i) trace.sample(0, 0, 1.5f, 30, -30, 30);
  ASSERT_EQ(trace.estimator.state(), Estimator::CALIBRATING);
  EXPECT_EQ(trace.estimator.tareBlockers(),
            Estimator::GYRO_X | Estimator::GYRO_Y | Estimator::GYRO_Z | Estimator::ACCEL_MAGNITUDE);
  EXPECT_GT(countLog("stationary_wait blocked=0x0f"), 0U);
  EXPECT_LE(countLog("stationary_wait"), 3U);
}

TEST_F(IMUTiltEstimatorTest, ReportsSampleTimeAndUnthrottledGaps) {
  Trace trace;
  trace.roll(30);
  EXPECT_EQ(trace.estimator.sampleIntervalMs(), 0U);
  EXPECT_EQ(countLog("sample t=1025 dt_ms=0"), 1U);
  trace.roll(30);
  EXPECT_EQ(trace.estimator.sampleIntervalMs(), 25U);
  trace.roll(30, 0, 0, 0, 125);
  EXPECT_EQ(trace.estimator.sampleIntervalMs(), 125U);
  EXPECT_EQ(countLog("t=1175 sample_gap dt_ms=125"), 1U);
  // A gap is reported even before the next periodic sample trace is due.
  EXPECT_EQ(countLog("[GYR-IMU] sample t="), 1U);
}

TEST_F(IMUTiltEstimatorTest, SampleTraceIsRateLimited) {
  Trace trace;
  trace.hold(30, 1000);
  EXPECT_EQ(countLog("[GYR-IMU] sample t="), 4U);
  EXPECT_EQ(countLog("estimate t="), 4U);
}

TEST_F(IMUTiltEstimatorTest, InvalidInputIsRejectedWithoutPoisoningAttitude) {
  Trace trace;
  trace.hold();
  for (int i = 0; i < 40; ++i) trace.sample(0, std::numeric_limits<float>::quiet_NaN(), 1);
  EXPECT_EQ(countLog("fault=invalid_sample"), 1U);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  trace.roll(30);
  EXPECT_EQ(countLog("reason=valid_samples_restored"), 1U);
}

TEST_F(IMUTiltEstimatorTest, BeginResetsDiagnosticAndPointerSessionState) {
  Trace trace;
  for (int i = 0; i < 20; ++i) trace.sample(0, 0, 1.5f, 30, 0, 0);
  ASSERT_NE(trace.estimator.tareBlockers(), 0);
  trace.estimator.begin();
  EXPECT_EQ(trace.estimator.state(), Estimator::CALIBRATING);
  EXPECT_EQ(trace.estimator.tareBlockers(), 0);
  EXPECT_EQ(trace.estimator.sampleIntervalMs(), 0U);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  const Move move = trace.poll();
  EXPECT_EQ(move.x, 0);
  EXPECT_EQ(move.y, 0);
  trace.hold();
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
}

TEST_F(IMUTiltEstimatorTest, FlatPositiveZTaresAndTracksFiniteMotion) {
  Trace trace;
  for (int i = 0; i < 120; ++i) trace.sample(0, 0, 1);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  for (int step = 1; step <= 40; ++step) trace.roll(step * 0.2f, 8);
  EXPECT_NEAR(trace.estimator.roll(), 8, 1);
  EXPECT_NE(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, FlatNegativeZTaresAndTracksFiniteMotion) {
  Trace trace;
  for (int i = 0; i < 120; ++i) trace.sample(0, 0, -1);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_TRUE(trace.estimator.hasFiniteState());
  for (int step = 1; step <= 40; ++step) trace.roll(180.0f + step * 0.2f, 8);
  EXPECT_NEAR(trace.estimator.roll(), 8, 1);
  EXPECT_NE(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, StationaryZBiasAllowsTracking) {
  Trace trace;
  trace.hold(30, 10000, 10, 3);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(0), 10, 0.01f);
  EXPECT_NEAR(trace.estimator.estimatedGyroBias(2), 3, 0.01f);
  EXPECT_EQ(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, UprightRotationAroundScreenAxisMovesPointer) {
  Trace trace;
  trace.hold(90);
  int events = 0;
  // Body Y is parallel to gravity: genuine rotation leaves the acceleration vector unchanged.
  for (int i = 0; i < 80; ++i) {
    trace.sample(0, 1, 0, 0, 10, 0);
    const Move move = trace.poll();
    if (move.x || move.y) ++events;
  }
  EXPECT_GT(events, 0);
}

TEST_F(IMUTiltEstimatorTest, SmallExcursionAcrossVerticalKeepsReference) {
  Trace trace;
  trace.hold(88);
  ASSERT_EQ(trace.estimator.state(), Estimator::TRACKING);
  for (int step = 1; step <= 8; ++step) trace.roll(88.0f + step * 0.5f, 20);
  bool lostTracking = trace.estimator.state() != Estimator::TRACKING;
  for (int i = 0; i < 200; ++i) {
    trace.roll(92);
    lostTracking |= trace.estimator.state() != Estimator::TRACKING;
  }
  EXPECT_FALSE(lostTracking) << "A four-degree excursion must not cause a new tare";
}

TEST_F(IMUTiltEstimatorTest, ClosedExcursionsAtDifferentSpeedsKeepNeutral) {
  Trace trace;
  trace.hold();
  bool lostTracking = false;
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (int step = 1; step <= 40; ++step) {
      trace.roll(30.0f + step * 0.25f, 10);
      lostTracking |= trace.estimator.state() != Estimator::TRACKING;
    }
    for (int step = 1; step <= 400; ++step) {
      trace.roll(40.0f - step * 0.025f, -1);
      lostTracking |= trace.estimator.state() != Estimator::TRACKING;
    }
  }
  trace.hold(30, 1000);
  EXPECT_FALSE(lostTracking);
  EXPECT_NEAR(trace.estimator.roll(), 0, 1);
  EXPECT_EQ(trace.poll().x, 0);
}

TEST_F(IMUTiltEstimatorTest, SlowLargePostureChangeRecentersAfterSettling) {
  Trace trace;
  trace.hold(20);
  for (int step = 1; step <= 1200; ++step) trace.roll(20.0f + step * 0.05f, 2);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.roll(), 60, 2);
  trace.hold(80, 500, 1.25f);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.roll(), 0, 2);
  EXPECT_EQ(trace.poll().x, 0);
  EXPECT_EQ(countLog("state=TRACKING->NEEDS_TARE reason=posture_change"), 1U);
}

TEST_F(IMUTiltEstimatorTest, BriefLargeExcursionKeepsOriginalNeutral) {
  Trace trace;
  trace.hold(20);
  for (int step = 1; step <= 200; ++step) trace.roll(20.0f + step * 0.25f, 10);
  trace.hold(70, 150);
  for (int step = 1; step <= 200; ++step) trace.roll(70.0f - step * 0.25f, -10);
  trace.hold(20, 1000);
  EXPECT_EQ(trace.estimator.state(), Estimator::TRACKING);
  EXPECT_NEAR(trace.estimator.roll(), 0, 1);
  EXPECT_EQ(trace.poll().x, 0);
  EXPECT_EQ(countLog("reason=posture_change"), 0U);
}

TEST_F(IMUTiltEstimatorTest, OneQuietSampleAfterMotionDoesNotEstablishRest) {
  Trace trace;
  for (int step = 1; step <= 32; ++step) trace.roll(20.0f + step, 40);
  ASSERT_NE(trace.estimator.state(), Estimator::TRACKING);
  trace.roll(52);
  EXPECT_NE(trace.estimator.state(), Estimator::TRACKING);
}

}  // namespace

void tiltTestLog(const char* level, const char* origin, const char* format, ...) {
  const size_t available = capturedLog.size() - capturedLogSize;
  const int prefixLength = snprintf(capturedLog.data() + capturedLogSize, available, "%s [%s] ", level, origin);
  if (prefixLength < 0 || static_cast<size_t>(prefixLength) >= available) {
    logTruncated = true;
    return;
  }
  capturedLogSize += static_cast<size_t>(prefixLength);
  va_list args;
  va_start(args, format);
  appendLog(format, args);
  va_end(args);
  if (capturedLogSize + 1 < capturedLog.size()) {
    capturedLog[capturedLogSize++] = '\n';
    capturedLog[capturedLogSize] = '\0';
  } else {
    logTruncated = true;
  }
}
