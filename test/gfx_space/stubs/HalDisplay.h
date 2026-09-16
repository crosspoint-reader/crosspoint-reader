#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>

// Measurement does not touch the display. These declarations let the complete
// production renderer compile; hardware-only methods are discarded by the linker.
class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Overlay, Absolute };
  enum class GrayscaleBase { Separate, Combined };
  struct GrayscaleCapabilities {
    bool asyncBase;
    bool stripUploads;
    GrayscaleBase base;
  };
  uint8_t* getFrameBuffer() const;
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;
  uint8_t* lendFrameBufferStorage(uint32_t*);
  void returnFrameBufferStorage();
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t) const;
  bool isInverted() const;
  void clearScreen(uint8_t) const;
  void displayBuffer(RefreshMode, bool);
  void displayBufferAsync(RefreshMode);
  void waitRefreshComplete();
  bool supportsAsyncRefresh() const;
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode) const;
  void displayGrayscaleBase(RefreshMode, bool);
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode, bool);
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t);
  void copyGrayscaleLsbBuffers(const uint8_t*);
  void copyGrayscaleMsbBuffers(const uint8_t*);
  void displayGrayBuffer(bool);
  void cleanupGrayscaleBuffers(const uint8_t*);
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t);
};
