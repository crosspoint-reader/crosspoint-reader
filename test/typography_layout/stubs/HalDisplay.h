#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 792;
  static constexpr uint16_t DISPLAY_HEIGHT = 528;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode : uint8_t { Overlay, Full };
  enum class GrayscaleBase : uint8_t { Separate, Combined };

  struct GrayscaleCapabilities {
    bool supported = true;
    bool stripUploads = false;
    bool asyncBase = false;
    GrayscaleBase base = GrayscaleBase::Separate;
  };

  HalDisplay() : buffer_(BUFFER_SIZE, 0xFF) {}

  uint8_t* getFrameBuffer() const { return lent_ ? nullptr : buffer_.data(); }
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut) {
    if (lent_) return nullptr;
    lent_ = true;
    if (sizeOut) *sizeOut = BUFFER_SIZE;
    return buffer_.data();
  }
  void returnFrameBufferStorage() {
    lent_ = false;
    std::memset(buffer_.data(), 0xFF, BUFFER_SIZE);
  }

  void clearScreen(uint8_t color = 0xFF) const { std::memset(buffer_.data(), color, BUFFER_SIZE); }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  bool isInverted() const { return false; }
  void setReaderRefresh(bool) {}
  bool displayRetainedGrayscaleOverlay(uint16_t, uint16_t, uint16_t, uint16_t, bool = false) { return false; }
  void controllerIdle() {}
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) {}
  void displayBufferAsync(RefreshMode = FAST_REFRESH) {}
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode = GrayscaleMode::Overlay) const { return {}; }
  void displayGrayscaleBase(RefreshMode = HALF_REFRESH, bool = false) {}
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode = HALF_REFRESH, bool = false) { return true; }
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) {}

 private:
  mutable std::vector<uint8_t> buffer_;
  bool lent_ = false;
};
