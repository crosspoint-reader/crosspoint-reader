#pragma once

class GfxRenderer;

// Utility for saving the current frame buffer as a BMP file on the SD card.
namespace ScreenCapture {

// Writes /screencap/<name>.bmp from the current frame buffer.
// The frame buffer is 800×480 (physical panel orientation), 1 bit per pixel.
// Returns true on success.
bool save(const GfxRenderer& renderer, const char* name);

}  // namespace ScreenCapture
