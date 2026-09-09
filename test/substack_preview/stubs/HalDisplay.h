#pragma once

// Host-build stand-in for lib/hal/HalDisplay.h, used only by the
// substack_preview tool. Real HalDisplay wraps a hardware EInkDisplay
// (SPI panel); this wraps a plain heap buffer and writes it to a BMP file on
// displayBuffer()/refreshDisplay() instead of pushing pixels to hardware.
// GfxRenderer.cpp only touches the methods below (grep-verified) — see
// firmware/CLAUDE.md's HAL pattern.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = static_cast<uint32_t>(DISPLAY_WIDTH_BYTES) * DISPLAY_HEIGHT;

  HalDisplay() { buffer_ = static_cast<uint8_t*>(std::malloc(BUFFER_SIZE)); std::memset(buffer_, 0xFF, BUFFER_SIZE); }
  ~HalDisplay() { std::free(buffer_); }

  void begin(bool = false) {}

  void clearScreen(uint8_t color = 0xFF) const { std::memset(buffer_, color, BUFFER_SIZE); }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  void drawImageTransparent(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}

  // Snapshot dump path: every call captures the current buffer under
  // outputPath_ so the preview tool can save each state as its own file
  // without the caller needing to know about HalDisplay internals.
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) { dump(); }
  void displayBufferAsync(RefreshMode = FAST_REFRESH) { dump(); }
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  void refreshDisplay(RefreshMode = FAST_REFRESH, bool = false) { dump(); }

  void setInverted(bool v) { inverted_ = v; }
  bool toggleInverted() { inverted_ = !inverted_; return inverted_; }
  bool isInverted() const { return inverted_; }

  void deepSleep() {}

  uint8_t* getFrameBuffer() const { return buffer_; }
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut) { if (sizeOut) *sizeOut = BUFFER_SIZE; return buffer_; }
  void returnFrameBufferStorage() {}

  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void displayGrayscaleBase(RefreshMode = HALF_REFRESH, bool = false) {}
  void copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}
  enum { GRAY_PLANE_LSB, GRAY_PLANE_MSB };
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}
  bool supportsStripGrayscale() const { return false; }
  bool combinesGrayscaleBase() const { return false; }

  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  // Preview-only hook: substack_preview sets this before each render so
  // displayBuffer() knows where to save the resulting screen.
  static void setNextOutputPath(const std::string& path) { nextOutputPath_ = path; }

 private:
  void dump() const;

  uint8_t* buffer_ = nullptr;
  bool inverted_ = false;
  static inline std::string nextOutputPath_;
};

extern HalDisplay display;
