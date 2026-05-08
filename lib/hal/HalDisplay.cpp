#include <HalDisplay.h>
#include <HalGPIO.h>

#if CROSSPOINT_EMULATED
#include <SDL.h>

// Global HalDisplay instance
HalDisplay display;

namespace {
uint8_t frameBuffer[HalDisplay::BUFFER_SIZE];
SDL_Window* window = nullptr;
SDL_Renderer* sdlRenderer = nullptr;
SDL_Texture* texture = nullptr;
constexpr uint16_t WINDOW_WIDTH = HalDisplay::DISPLAY_HEIGHT;
constexpr uint16_t WINDOW_HEIGHT = HalDisplay::DISPLAY_WIDTH;

void ensureSdl() {
  if (window) return;
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  window = SDL_CreateWindow("CrossPoint Reader Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH,
                            WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
  sdlRenderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  texture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH,
                              WINDOW_HEIGHT);
}
}  // namespace

HalDisplay::HalDisplay() { memset(frameBuffer, 0xFF, sizeof(frameBuffer)); }
HalDisplay::~HalDisplay() {}
void HalDisplay::begin() { ensureSdl(); }
void HalDisplay::clearScreen(uint8_t color) const { memset(frameBuffer, color, sizeof(frameBuffer)); }
void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  (void)fromProgmem;
  for (uint16_t yy = 0; yy < h; yy++) {
    if (y + yy >= DISPLAY_HEIGHT) break;
    const uint16_t srcBytes = (w + 7) / 8;
    const uint16_t dstBytes = DISPLAY_WIDTH_BYTES;
    if (x == 0 && w == DISPLAY_WIDTH) {
      memcpy(frameBuffer + (y + yy) * dstBytes, imageData + yy * srcBytes, std::min<uint16_t>(srcBytes, dstBytes));
    } else {
      for (uint16_t xx = 0; xx < w; xx++) {
        if (x + xx >= DISPLAY_WIDTH) break;
        const bool black = (imageData[yy * srcBytes + (xx >> 3)] & (0x80 >> (xx & 7))) == 0;
        uint8_t& b = frameBuffer[(y + yy) * dstBytes + ((x + xx) >> 3)];
        const uint8_t mask = 0x80 >> ((x + xx) & 7);
        if (black)
          b &= ~mask;
        else
          b |= mask;
      }
    }
  }
}
void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  drawImage(imageData, x, y, w, h, fromProgmem);
}
void HalDisplay::displayBuffer(HalDisplay::RefreshMode, bool) {
  ensureSdl();
  uint32_t* pixels = nullptr;
  int pitch = 0;
  if (SDL_LockTexture(texture, nullptr, reinterpret_cast<void**>(&pixels), &pitch) == 0) {
    for (uint16_t y = 0; y < WINDOW_HEIGHT; y++) {
      auto* row = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pixels) + y * pitch);
      for (uint16_t x = 0; x < WINDOW_WIDTH; x++) {
        const uint16_t physicalX = y;
        const uint16_t physicalY = DISPLAY_HEIGHT - 1 - x;
        const bool white =
            (frameBuffer[physicalY * DISPLAY_WIDTH_BYTES + (physicalX >> 3)] & (0x80 >> (physicalX & 7))) != 0;
        row[x] = white ? 0xFFFFFFFF : 0xFF111111;
      }
    }
    SDL_UnlockTexture(texture);
  }
  SDL_RenderClear(sdlRenderer);
  SDL_RenderCopy(sdlRenderer, texture, nullptr, nullptr);
  SDL_RenderPresent(sdlRenderer);
}
void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  displayBuffer(mode, turnOffScreen);
}
void HalDisplay::deepSleep() {}
uint8_t* HalDisplay::getFrameBuffer() const { return frameBuffer; }
void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t*) {
  memcpy(frameBuffer, lsbBuffer, BUFFER_SIZE);
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { memcpy(frameBuffer, lsbBuffer, BUFFER_SIZE); }
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { memcpy(frameBuffer, msbBuffer, BUFFER_SIZE); }
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}
void HalDisplay::displayGrayBuffer(bool) {
  // The firmware uses display-specific grayscale planes/LUTs here. In the emulator,
  // showing either plane directly looks like a black mask with dotted text, so keep
  // the already-presented BW frame until we emulate grayscale composition properly.
}
uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

#else

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin() {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  // Request resync after specific wakeup events to ensure clean display state
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
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
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

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

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
#endif
