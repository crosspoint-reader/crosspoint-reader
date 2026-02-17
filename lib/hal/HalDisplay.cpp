#include <HalDisplay.h>
#include <HalGPIO.h>

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
  // Set X3-specific display dimensions before initializing
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayDimensions(792, 528);
  }

  einkDisplay.begin();

  // Request resync after specific wakeup events to ensure clean display state
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton ||
      wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::setDisplayDimensions(uint16_t width, uint16_t height) {
  einkDisplay.setDisplayDimensions(width, height);
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
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
  if (gpio.deviceIsX3() && (lastBufferWasGray || mode == RefreshMode::HALF_REFRESH)) {
    einkDisplay.requestResync(1);
  }
  lastBufferWasGray = false;
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
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
  if (gpio.deviceIsX3() && !lastBufferWasGray) {
    einkDisplay.requestResync(1);
  }
  lastBufferWasGray = true;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
