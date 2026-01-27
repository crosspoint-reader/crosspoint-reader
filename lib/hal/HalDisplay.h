#pragma once
#include <EInkDisplay.h>
#include <Arduino.h>
#include <SPI.h>

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);

  // Destructor
  ~HalDisplay();

  // Refresh modes (guarded to avoid redefinition in test builds)
  using RefreshMode = EInkDisplay::RefreshMode;
  static constexpr EInkDisplay::RefreshMode FULL_REFRESH = EInkDisplay::FULL_REFRESH;
  static constexpr EInkDisplay::RefreshMode HALF_REFRESH = EInkDisplay::HALF_REFRESH;
  static constexpr EInkDisplay::RefreshMode FAST_REFRESH = EInkDisplay::FAST_REFRESH;

  // Initialize the display hardware and driver
  void begin();

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer();

 private:
  bool is_emulated = CROSSPOINT_EMULATED;

  // real display implementation
  EInkDisplay einkDisplay;

  // emulated display implementation
  uint8_t* emuFramebuffer0 = nullptr;
};
