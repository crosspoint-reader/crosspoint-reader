#pragma once

struct HalTiltSensor {
  bool wasTiltedForward() const { return false; }
  bool wasTiltedBack() const { return false; }
};

inline HalTiltSensor halTiltSensor;
