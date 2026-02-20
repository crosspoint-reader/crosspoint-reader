#include <HalDisplay.h>
#include <HalGPIO.h>

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::x3CaptureFrameSample() {
  if (!gpio.deviceIsX3()) {
    return;
  }
  const uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  const uint32_t bufferSize = einkDisplay.getBufferSize();
  if (!frameBuffer || bufferSize == 0) {
    x3FrameSample.clear();
    x3FrameSampleValid = false;
    return;
  }

  const size_t sampleCount = (bufferSize + X3_FRAME_SAMPLE_STRIDE - 1) / X3_FRAME_SAMPLE_STRIDE;
  if (x3FrameSample.size() != sampleCount) {
    x3FrameSample.assign(sampleCount, 0);
  }

  for (size_t i = 0; i < sampleCount; ++i) {
    const size_t offset = i * X3_FRAME_SAMPLE_STRIDE;
    x3FrameSample[i] = frameBuffer[offset < bufferSize ? offset : (bufferSize - 1)];
  }
  x3FrameSampleValid = true;
}

bool HalDisplay::x3DetectLargeFrameDelta() {
  if (!gpio.deviceIsX3()) {
    return false;
  }
  const uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  const uint32_t bufferSize = einkDisplay.getBufferSize();
  if (!frameBuffer || bufferSize == 0) {
    x3FrameSampleValid = false;
    return false;
  }

  const size_t sampleCount = (bufferSize + X3_FRAME_SAMPLE_STRIDE - 1) / X3_FRAME_SAMPLE_STRIDE;
  if (!x3FrameSampleValid || x3FrameSample.size() != sampleCount) {
    x3CaptureFrameSample();
    return false;
  }

  size_t diffCount = 0;
  for (size_t i = 0; i < sampleCount; ++i) {
    const size_t offset = i * X3_FRAME_SAMPLE_STRIDE;
    const uint8_t current = frameBuffer[offset < bufferSize ? offset : (bufferSize - 1)];
    if (current != x3FrameSample[i]) {
      diffCount++;
    }
  }

  x3CaptureFrameSample();
  const size_t diffPercent = (diffCount * 100U) / sampleCount;
  return diffPercent >= X3_LARGE_DELTA_PERCENT;
}

HalDisplay::RefreshMode HalDisplay::applyX3RefreshPolicy(HalDisplay::RefreshMode mode) {
  if (!gpio.deviceIsX3()) {
    return mode;
  }
  const unsigned long now = millis();

  if (mode == RefreshMode::FAST_REFRESH) {
    if (x3DetectLargeFrameDelta()) {
      x3FastRefreshStreak = 0;
      x3LastRefreshMs = now;
      return RefreshMode::HALF_REFRESH;
    }

    const bool spacedUpdate = (x3LastRefreshMs == 0) || ((now - x3LastRefreshMs) >= X3_MIN_FAST_REFRESH_GAP_MS);
    if (spacedUpdate) {
      x3FastRefreshStreak++;
    }
    x3LastRefreshMs = now;

    if (x3FastRefreshStreak >= X3_MAX_FAST_REFRESH_STREAK) {
      x3FastRefreshStreak = 0;
      return RefreshMode::HALF_REFRESH;
    }
    return mode;
  }

  x3FastRefreshStreak = 0;
  x3LastRefreshMs = now;
  x3CaptureFrameSample();
  return mode;
}

void HalDisplay::begin() {
  // Set X3-specific display dimensions before initializing
  static constexpr uint16_t X3_DISPLAY_WIDTH = 792;
  static constexpr uint16_t X3_DISPLAY_HEIGHT = 528;
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayDimensions(X3_DISPLAY_WIDTH, X3_DISPLAY_HEIGHT);
  }

  einkDisplay.begin();

  // Request resync after specific wakeup events to ensure clean display state
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }

  x3FastRefreshStreak = 0;
  x3LastRefreshMs = 0;
  x3FrameSample.clear();
  x3FrameSampleValid = false;
  x3CaptureFrameSample();
}

void HalDisplay::setDisplayDimensions(uint16_t width, uint16_t height) {
  einkDisplay.setDisplayDimensions(width, height);
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  mode = applyX3RefreshPolicy(mode);

  if (gpio.deviceIsX3() && (lastBufferWasGray || mode == RefreshMode::HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }

  lastBufferWasGray = false;
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  mode = applyX3RefreshPolicy(mode);

  if (gpio.deviceIsX3() && (lastBufferWasGray || mode == RefreshMode::HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }

  lastBufferWasGray = false;
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  if (gpio.deviceIsX3()) {
    // Hard-disable grayscale display passes on X3 until native LUT path is stable.
    lastBufferWasGray = false;
    return;
  }
  lastBufferWasGray = true;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
