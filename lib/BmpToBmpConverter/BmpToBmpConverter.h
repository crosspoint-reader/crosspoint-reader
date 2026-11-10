#pragma once

#include <cstdint>

class HalFile;
class Print;

class BmpToBmpConverter {
 public:
  // Scales a BMP to the portrait display the way book covers are converted: crop or fit, then one Atkinson
  // pass into a 2-bit BMP. 1-bit sources stay 1-bit so B/W images keep the B/W refresh path.
  // toneCurve (optional, 256 entries): maps each source gray before dithering — the inverse of the panel's measured
  // response, so a flat image lands on screen as drawn. Ignored for 1-bit output.
  static bool bmpFileToBmpStream(HalFile& bmpFile, Print& bmpOut, bool crop, bool originalThresholds,
                                 const uint8_t* toneCurve = nullptr, int targetWidth = 0, int targetHeight = 0);
  // targetWidth/targetHeight (optional): fit into this box instead of the portrait display (e.g. a rotated viewer).
};
