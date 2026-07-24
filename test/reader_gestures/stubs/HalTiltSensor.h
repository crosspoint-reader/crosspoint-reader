#pragma once

class HalTiltSensor {
 public:
  bool wasTiltedForward() { return false; }
  bool wasTiltedBack() { return false; }
};

extern HalTiltSensor halTiltSensor;
