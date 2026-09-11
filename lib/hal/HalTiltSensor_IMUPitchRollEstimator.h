#include "HalTiltSensor.h"

class HalTiltSensor::IMUPitchRollEstimator {
  enum State : uint8_t { CALIBRATING = 0, NEEDS_TARE = 1, TRACKING = 2 };

  State _state;

  float _pitch, _roll;
  float _mTare[9];
  float _accRotation[2];
  float _pifBias[2], _pifEstimate[2];
  uint8_t _pifCalibSteps : 7, _azNeg : 1;
  unsigned long _lastPIFUpdateMs = 0, _lastAngleUpdateMs = 0, _lastMoveXMs = 0, _lastMoveYMs = 0;

  // Tuning constants
  static constexpr float COMPLEMENTARY_FACTOR = .9f;  // complementary alpha for sensor fusion
  static constexpr float GYRO_ANGULAR_THRESH =
      2.5f;  // absolute threshold below which deg/sec readings from gyro are ignored as noise
  static constexpr float GYRO_TARE_MAX_ANG =
      20.0f;  // accumulated angular deviation from tare position after which re-taring happens
  static constexpr float TRACK_MIN_ANGLE =
      2.5f;  // minimum angle deviation from tare position to trigger up/down left/right movement
  static constexpr float MOVE_REPEAT_MIN_MS =
      150.0f;  // ms to trigger repeated move events, scales between 1x and 3x this, depending on the magnitude of the
               // angular tilt (more tilt -> faster repeat)
  static constexpr float PIF_KP = 2.75f;  // Kp value for gyro PI predictor filter
  static constexpr float PIF_KI_BOOST =
      32.0f;  // High Ki gain for gyro PI predictor during CALIBRATION state to converge quickly
  static constexpr float PIF_KI_NORMAL = .6f;       // Much lower Ki gain for gyro PI predictor after calibration
  static constexpr uint8_t CALIBRATION_STEPS = 25;  // Amount of samples needed for CALIBRATION step to converge
                                                    // reasonably (at 50Hz, 25 samples take about 500ms)

  static constexpr float SENSITIVITY_FACTOR_LOW = 2.25f;
  static constexpr float SENSITIVITY_FACTOR_NORMAL = 1.5f;
  static constexpr float SENSITIVITY_FACTOR_HIGH = 1.0f;

  // resets tareing matrix to identity
  void _resetTare();

  // calculates and sets tareing matrix from current sensor orientation.
  bool _tare(float ax, float ay, float az, float gx, float gy, float gz);

  // applies the tareing matrix by multiplication
  void _applyTare(float& x, float& y, float& z);

  // get pitch and roll angles from accelerometer data
  void _accelPitchRoll(float& pitch, float& roll, float ax, float ay, float az);

  // updates the PI predictor/observer filter to estimate gyro bias
  void _updatePIF(float ax, float ay, float az, float gx, float gy, float gz, unsigned long timestamp);

  // advances the pitch/roll model with a new acceleration and gyro sample
  void _updateAngles(float ax, float ay, float az, float gx, float gy, float gz, unsigned long timestamp);

  // helper for pollPointerMove() to obtain current pointer move event on an axis
  void _getPointerMove(float angle, int& dir, unsigned long& lastMillis, bool invert, const float sensitivityFactor);

 public:
  float pitch() { return _pitch; }
  float roll() { return _roll; }

  // Call after instantiaton.
  void begin();

  // Consumes a single IMU update with (a)cceleration and (g)yro values, as
  // well as the current timestamp in milliseconds.
  void consume(float ax, float ay, float az, float gx, float gy, float gz, unsigned long timestamp);

  // Poll whether a horizontal or vertical "move pointer" event shall be generated, according to internal state.
  // Sensitivity is 0-2, for low - normal - high
  void pollPointerMove(int& moveX, int& moveY, uint8_t orientation, uint8_t sensitivity, bool invertX, bool invertY);
};
