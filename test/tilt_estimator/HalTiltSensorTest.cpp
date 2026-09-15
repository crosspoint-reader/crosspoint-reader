#include <gtest/gtest.h>

#include <cmath>
#include <cstdarg>
#include <numbers>

#include "HalTiltSensor.h"

namespace {

constexpr uint8_t POINTER_MODE = CrossPointTiltSensorMode::TILT_POINTER_ACTIVE;
constexpr uint8_t PAGE_MODE = CrossPointTiltSensorMode::TILT_PAGE_ACTIVE;
constexpr float RADIANS_PER_DEGREE = std::numbers::pi_v<float> / 180.0f;

unsigned long nowMs = 0;

class HalTiltSensorTest : public testing::Test {
 protected:
  HalTiltSensor sensor;

  void SetUp() override {
    nowMs = 0;
    Imu::reset();
    sensor.begin();
  }

  void TearDown() override { sensor.update(CrossPointTiltSensorMode::SENSOR_OFF, CrossPointOrientation::PORTRAIT); }

  void wake(uint8_t mode = POINTER_MODE) {
    sensor.update(mode, CrossPointOrientation::PORTRAIT);
    nowMs = 400;
  }

  void sample(uint8_t mode, float ax = 0, float ay = 0, float az = 1, float gx = 0, float gy = 0, float gz = 0,
              uint32_t intervalMs = 25, uint8_t orientation = CrossPointOrientation::PORTRAIT) {
    nowMs += intervalMs;
    Imu::nextSample = {ax, ay, az, gx, gy, gz};
    sensor.update(mode, orientation);
  }

  void calibrate() {
    wake();
    for (int i = 0; i < 30; ++i) {
      sample(POINTER_MODE);
      EXPECT_FALSE(sensor.hadActivity());
    }
  }
};

TEST_F(HalTiltSensorTest, StationaryPointerSamplingDoesNotReportActivity) {
  calibrate();
  for (int i = 0; i < 80; ++i) {
    sample(POINTER_MODE);
    EXPECT_FALSE(sensor.hadActivity());
  }
}

TEST_F(HalTiltSensorTest, GeneratedPointerMoveReportsActivityAndIsConsumedOnce) {
  calibrate();

  bool generated = false;
  for (int step = 1; step <= 40; ++step) {
    const float angle = step * 0.2f * RADIANS_PER_DEGREE;
    sample(POINTER_MODE, 0, sinf(angle), cosf(angle), 8);
    if (sensor.hadActivity()) {
      generated = true;
      break;
    }
  }
  ASSERT_TRUE(generated);

  int moveX = 0;
  int moveY = 0;
  EXPECT_TRUE(sensor.getXYPointerMove(moveX, moveY));
  EXPECT_NE(moveX, 0);
  EXPECT_EQ(moveY, 0);
  EXPECT_FALSE(sensor.getXYPointerMove(moveX, moveY));
  EXPECT_EQ(moveX, 0);
  EXPECT_EQ(moveY, 0);
}

TEST_F(HalTiltSensorTest, LeavingReaderModeClearsUnconsumedPageTurn) {
  wake(PAGE_MODE);
  nowMs = 700;
  sample(PAGE_MODE, 0, 0, 1, 300);
  ASSERT_TRUE(sensor.hadActivity());

  sample(POINTER_MODE);
  EXPECT_FALSE(sensor.wasTiltedForward());
  EXPECT_FALSE(sensor.wasTiltedBack());
}

TEST_F(HalTiltSensorTest, PointerMoveUsesTheCurrentScreenOrientation) {
  calibrate();

  bool generated = false;
  for (int step = 1; step <= 40; ++step) {
    const float angle = step * 0.2f * RADIANS_PER_DEGREE;
    sample(POINTER_MODE, 0, sinf(angle), cosf(angle), 8, 0, 0, 25, CrossPointOrientation::LANDSCAPE_CW);
    if (sensor.hadActivity()) {
      generated = true;
      break;
    }
  }
  ASSERT_TRUE(generated);

  int moveX = 0;
  int moveY = 0;
  EXPECT_TRUE(sensor.getXYPointerMove(moveX, moveY));
  EXPECT_EQ(moveX, 0);
  EXPECT_NE(moveY, 0);
}

TEST_F(HalTiltSensorTest, LearnedBiasAllowsImmediateMovementInNextPointerSession) {
  calibrate();
  sample(PAGE_MODE);

  sample(POINTER_MODE, 0, 0, 1, 16);
  bool generated = false;
  for (int step = 1; step <= 20; ++step) {
    const float angle = step * 0.4f * RADIANS_PER_DEGREE;
    sample(POINTER_MODE, 0, sinf(angle), cosf(angle), 16);
    if (sensor.hadActivity()) {
      generated = true;
      break;
    }
  }

  EXPECT_TRUE(generated);
}

}  // namespace

unsigned long millis() { return nowMs; }

void tiltTestLog(const char*, const char*, const char*, ...) {}
