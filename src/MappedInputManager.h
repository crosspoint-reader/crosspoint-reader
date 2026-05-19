#pragma once

#include <HalGPIO.h>

#include <map>

#include "HalTiltSensor.h"

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    TiltLeft,
    TiltRight,
    TiltUp,
    TiltDown,
    RotateLeft,
    RotateRight
  };
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio, HalTiltSensor& tiltSensor) : gpio(gpio), tiltSensor(tiltSensor) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  bool wasTilted(const Button button) const;
  bool wasAnyTilted() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  HalTiltSensor& tiltSensor;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool mapTilt(Button button, bool (HalTiltSensor::*fn)(uint8_t)) const;

  // Map the gyro axis to left/right tilt based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  const std::map<Button, uint8_t> PortraitTiltNormal = {
      {Button::TiltRight, HalTiltSensor::TILT_X_POS},   {Button::TiltLeft, HalTiltSensor::TILT_X_NEG},
      {Button::TiltUp, HalTiltSensor::TILT_Y_NEG},      {Button::TiltDown, HalTiltSensor::TILT_Y_POS},
      {Button::RotateRight, HalTiltSensor::TILT_Z_POS}, {Button::RotateLeft, HalTiltSensor::TILT_Z_NEG}};

  const std::map<Button, uint8_t> PortraitTiltInverted = {
      {Button::TiltRight, HalTiltSensor::TILT_X_NEG},   {Button::TiltLeft, HalTiltSensor::TILT_X_POS},
      {Button::TiltUp, HalTiltSensor::TILT_Y_POS},      {Button::TiltDown, HalTiltSensor::TILT_Y_NEG},
      {Button::RotateRight, HalTiltSensor::TILT_Z_NEG}, {Button::RotateLeft, HalTiltSensor::TILT_Z_POS}};

  const std::map<Button, uint8_t> InvertTiltNormal = {
      {Button::TiltRight, HalTiltSensor::TILT_X_NEG},   {Button::TiltLeft, HalTiltSensor::TILT_X_POS},
      {Button::TiltUp, HalTiltSensor::TILT_Y_POS},      {Button::TiltDown, HalTiltSensor::TILT_Y_NEG},
      {Button::RotateRight, HalTiltSensor::TILT_Z_POS}, {Button::RotateLeft, HalTiltSensor::TILT_Z_NEG}};

  const std::map<Button, uint8_t> InvertTiltInverted = {
      {Button::TiltRight, HalTiltSensor::TILT_X_POS},   {Button::TiltLeft, HalTiltSensor::TILT_X_NEG},
      {Button::TiltUp, HalTiltSensor::TILT_Y_NEG},      {Button::TiltDown, HalTiltSensor::TILT_Y_POS},
      {Button::RotateRight, HalTiltSensor::TILT_Z_NEG}, {Button::RotateLeft, HalTiltSensor::TILT_Z_POS}};

  const std::map<Button, uint8_t> LandscapeCWTiltNormal = {
      {Button::TiltRight, HalTiltSensor::TILT_Y_NEG},   {Button::TiltLeft, HalTiltSensor::TILT_Y_POS},
      {Button::TiltUp, HalTiltSensor::TILT_X_POS},      {Button::TiltDown, HalTiltSensor::TILT_X_NEG},
      {Button::RotateRight, HalTiltSensor::TILT_Z_POS}, {Button::RotateLeft, HalTiltSensor::TILT_Z_NEG}};

  const std::map<Button, uint8_t> LandscapeCWTiltInverted = {
      {Button::TiltRight, HalTiltSensor::TILT_Y_POS},   {Button::TiltLeft, HalTiltSensor::TILT_Y_NEG},
      {Button::TiltUp, HalTiltSensor::TILT_X_NEG},      {Button::TiltDown, HalTiltSensor::TILT_X_POS},
      {Button::RotateRight, HalTiltSensor::TILT_Z_NEG}, {Button::RotateLeft, HalTiltSensor::TILT_Z_POS}};

  const std::map<Button, uint8_t> LandscapeCCWTiltNormal = {
      {Button::TiltRight, HalTiltSensor::TILT_Y_POS},   {Button::TiltLeft, HalTiltSensor::TILT_Y_NEG},
      {Button::TiltUp, HalTiltSensor::TILT_X_NEG},      {Button::TiltDown, HalTiltSensor::TILT_X_POS},
      {Button::RotateRight, HalTiltSensor::TILT_Z_POS}, {Button::RotateLeft, HalTiltSensor::TILT_Z_NEG}};

  const std::map<Button, uint8_t> LandscapeCCWTiltInverted = {
      {Button::TiltRight, HalTiltSensor::TILT_Y_NEG},   {Button::TiltLeft, HalTiltSensor::TILT_Y_POS},
      {Button::TiltUp, HalTiltSensor::TILT_X_POS},      {Button::TiltDown, HalTiltSensor::TILT_X_NEG},
      {Button::RotateRight, HalTiltSensor::TILT_Z_NEG}, {Button::RotateLeft, HalTiltSensor::TILT_Z_POS}};
};
