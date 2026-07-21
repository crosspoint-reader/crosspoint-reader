#include "HalDisplay.h"

#include <GfxRenderer.h>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <vector>

#include "SimulatorControls.h"

static SDL_Window *window = nullptr;
static SDL_Renderer *sdl_renderer = nullptr;
static SDL_Texture *texture = nullptr;

// Pixel buffer written by the render task, read by the main thread for
// SDL_RenderPresent. On macOS, SDL calls must happen on the main thread.
static uint32_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::mutex pixelMutex;
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by
// shouldQuit().
std::atomic<bool> quitRequested{false};

static int currentWindowWidth = 0;
static int currentWindowHeight = 0;

namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
};

constexpr uint8_t kGrayWhite = 255;
constexpr uint8_t kGrayLight = 200;
constexpr uint8_t kGrayDark = 96;
constexpr uint8_t kGrayBlack = 0;

GrayscalePreviewState grayscalePreviewState;
std::array<uint8_t, HalDisplay::BUFFER_SIZE> frameBufferStorage{};
bool frameBufferLent = false;

uint32_t argbGray(uint8_t level) {
  return 0xFF000000u | (static_cast<uint32_t>(level) << 16) |
         (static_cast<uint32_t>(level) << 8) | level;
}

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * HalDisplay::DISPLAY_WIDTH + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] =
          getBit(fb, x, y) ? 0xFFFFFFFFu : 0xFF000000u;
    }
  }
  pendingPresent.store(true);
}

void clearGrayscalePlanes() {
  grayscalePreviewState.lsbPlane.fill(0);
  grayscalePreviewState.msbPlane.fill(0);
  grayscalePreviewState.lsbValid = false;
  grayscalePreviewState.msbValid = false;
}

void snapshotBwBase(const uint8_t *fb) {
  memcpy(grayscalePreviewState.bwBase.data(), fb, HalDisplay::BUFFER_SIZE);
  grayscalePreviewState.bwBaseValid = true;
  clearGrayscalePlanes();
}

void copyPlane(std::array<uint8_t, HalDisplay::BUFFER_SIZE> &dst,
               const uint8_t *src, bool &valid) {
  if (!src) {
    valid = false;
    dst.fill(0);
    return;
  }
  memcpy(dst.data(), src, HalDisplay::BUFFER_SIZE);
  valid = true;
}

void composeGrayscalePreview() {
  const uint8_t *bwBase = grayscalePreviewState.bwBaseValid
                              ? grayscalePreviewState.bwBase.data()
                              : frameBufferStorage.data();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool baseWhite = getBit(bwBase, x, y);
      const bool lsbActive = grayscalePreviewState.lsbValid &&
                             getBit(grayscalePreviewState.lsbPlane.data(), x, y);
      const bool msbActive = grayscalePreviewState.msbValid &&
                             getBit(grayscalePreviewState.msbPlane.data(), x, y);

      uint8_t level = kGrayWhite;
      if (!baseWhite) {
        if (msbActive) {
          level = lsbActive ? kGrayDark : kGrayLight;
        } else if (lsbActive) {
          level = kGrayDark;
        } else {
          level = kGrayBlack;
        }
      }

      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = argbGray(level);
    }
  }
  pendingPresent.store(true);
}

std::filesystem::path screenshotDirectory() {
  const char *configured = std::getenv("CROSSVI_SIM_SCREENSHOT_DIR");
  if (!configured || !*configured) {
    configured = std::getenv("CROSSPOINT_SIM_SCREENSHOT_DIR");
  }
  return configured && *configured ? std::filesystem::path(configured)
                                   : std::filesystem::path("simulator-screenshots");
}

uint32_t orientedPixel(const std::vector<uint32_t> &pixels,
                       GfxRenderer::Orientation orientation, int x, int y) {
  int sourceX = x;
  int sourceY = y;
  switch (orientation) {
  case GfxRenderer::Portrait:
    sourceX = y;
    sourceY = HalDisplay::DISPLAY_HEIGHT - 1 - x;
    break;
  case GfxRenderer::PortraitInverted:
    sourceX = HalDisplay::DISPLAY_WIDTH - 1 - y;
    sourceY = x;
    break;
  case GfxRenderer::LandscapeClockwise:
    sourceX = HalDisplay::DISPLAY_WIDTH - 1 - x;
    sourceY = HalDisplay::DISPLAY_HEIGHT - 1 - y;
    break;
  default:
    break;
  }
  return pixels[sourceY * HalDisplay::DISPLAY_WIDTH + sourceX];
}

void saveScreenshot(GfxRenderer::Orientation orientation) {
  const bool portrait = orientation == GfxRenderer::Portrait ||
                        orientation == GfxRenderer::PortraitInverted;
  const int width = portrait ? HalDisplay::DISPLAY_HEIGHT
                             : HalDisplay::DISPLAY_WIDTH;
  const int height = portrait ? HalDisplay::DISPLAY_WIDTH
                              : HalDisplay::DISPLAY_HEIGHT;

  std::vector<uint32_t> pixels(std::size(pixelBuf));
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> rawFrame{};
  {
    std::lock_guard<std::mutex> lock(pixelMutex);
    std::copy(std::begin(pixelBuf), std::end(pixelBuf), pixels.begin());
    if (grayscalePreviewState.bwBaseValid) {
      rawFrame = grayscalePreviewState.bwBase;
    } else {
      rawFrame = frameBufferStorage;
    }
  }

  std::error_code directoryError;
  const std::filesystem::path directory = screenshotDirectory();
  std::filesystem::create_directories(directory, directoryError);
  if (directoryError) {
    std::cerr << "[SIM] Could not create screenshot directory: "
              << directoryError.message() << std::endl;
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch())
                                .count() %
                            1000;
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm localTime{};
#if defined(_WIN32)
  localtime_s(&localTime, &time);
#else
  localtime_r(&time, &localTime);
#endif
  std::ostringstream name;
#if defined(SIMULATOR_DEVICE_X3)
  name << "crossvi-x3-";
#else
  name << "crossvi-x4-";
#endif
  name << std::put_time(&localTime, "%Y%m%d-%H%M%S") << '-' << std::setw(3)
       << std::setfill('0') << milliseconds;

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
      0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) {
    std::cerr << "[SIM] Could not allocate screenshot: " << SDL_GetError()
              << std::endl;
    return;
  }
  for (int y = 0; y < height; ++y) {
    auto *row = reinterpret_cast<uint32_t *>(
        static_cast<uint8_t *>(surface->pixels) + y * surface->pitch);
    for (int x = 0; x < width; ++x) {
      row[x] = orientedPixel(pixels, orientation, x, y);
    }
  }

  const std::filesystem::path bmpPath = directory / (name.str() + ".bmp");
  const std::filesystem::path rawPath =
      directory / (name.str() + ".framebuffer.bin");
  const int saveResult = SDL_SaveBMP(surface, bmpPath.string().c_str());
  SDL_FreeSurface(surface);

  std::ofstream raw(rawPath, std::ios::binary | std::ios::trunc);
  raw.write(reinterpret_cast<const char *>(rawFrame.data()), rawFrame.size());
  raw.close();
  if (saveResult != 0 || !raw) {
    std::cerr << "[SIM] Screenshot failed: " << SDL_GetError() << std::endl;
    return;
  }
  std::cout << "[SIM] Screenshot: " << bmpPath << " (" << width << 'x'
            << height << "), raw framebuffer: " << rawPath << std::endl;
}

} // namespace

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

static void getLogicalScreenSize(GfxRenderer::Orientation orientation,
                                 int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  *width = isPortrait ? HalDisplay::DISPLAY_HEIGHT : HalDisplay::DISPLAY_WIDTH;
  *height = isPortrait ? HalDisplay::DISPLAY_WIDTH : HalDisplay::DISPLAY_HEIGHT;
}

static void applyWindowGeometryIfNeeded(GfxRenderer::Orientation orientation) {
  if (!window || !sdl_renderer)
    return;

  int screenWidth = 0;
  int screenHeight = 0;
  getLogicalScreenSize(orientation, &screenWidth, &screenHeight);
  const SimulatorControls::Layout layout =
      SimulatorControls::makeLayout(screenWidth, screenHeight);
  SimulatorControls::setLayout(layout);
  if (layout.windowWidth == currentWindowWidth &&
      layout.windowHeight == currentWindowHeight)
    return;

  SDL_SetWindowSize(window, layout.windowWidth, layout.windowHeight);
  SDL_RenderSetLogicalSize(sdl_renderer, layout.windowWidth,
                           layout.windowHeight);
  currentWindowWidth = layout.windowWidth;
  currentWindowHeight = layout.windowHeight;
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

#if defined(SIMULATOR_DEVICE_X3)
static constexpr const char *WINDOW_TITLE = "CrossVi Simulator - XTEINK X3";
static constexpr const char *DEVICE_NAME = "X3";
#else
static constexpr const char *WINDOW_TITLE = "CrossVi Simulator - XTEINK X4";
static constexpr const char *DEVICE_NAME = "X4";
#endif

void HalDisplay::begin() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError()
              << std::endl;
    quitRequested.store(true);
    return;
  }

  int screenWidth = 0;
  int screenHeight = 0;
  extern GfxRenderer renderer;
  getLogicalScreenSize(renderer.getOrientation(), &screenWidth, &screenHeight);
  const SimulatorControls::Layout layout =
      SimulatorControls::makeLayout(screenWidth, screenHeight);
  SimulatorControls::setLayout(layout);

  // A strict nearest-neighbour path is the default. This keeps every firmware
  // framebuffer pixel discrete even on a scaled HiDPI desktop.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, layout.windowWidth,
                            layout.windowHeight,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    std::cerr << "SDL window creation failed: " << SDL_GetError() << std::endl;
    quitRequested.store(true);
    return;
  }
  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!sdl_renderer) {
    sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!sdl_renderer) {
    std::cerr << "SDL renderer creation failed: " << SDL_GetError() << std::endl;
    quitRequested.store(true);
    return;
  }

  SDL_RenderSetLogicalSize(sdl_renderer, layout.windowWidth,
                           layout.windowHeight);
  currentWindowWidth = layout.windowWidth;
  currentWindowHeight = layout.windowHeight;

  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);
  if (!texture) {
    std::cerr << "SDL texture creation failed: " << SDL_GetError() << std::endl;
    quitRequested.store(true);
    return;
  }
#if SDL_VERSION_ATLEAST(2, 0, 12)
  SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
#endif
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  memset(getFrameBuffer(), color, BUFFER_SIZE);
}

void HalDisplay::drawImage(const uint8_t *imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t *imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
}

void HalDisplay::displayWindow(int, int, int, int) {
  refreshDisplay(RefreshMode::FAST_REFRESH, false);
}

// Called from the render task (background thread): convert framebuffer to
// pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
  const uint8_t *fb = getFrameBuffer();
  if (!fb) return;
  std::lock_guard<std::mutex> lock(pixelMutex);
  snapshotBwBase(fb);
  renderBwPixels(fb);
}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
  const bool frameChanged = pendingPresent.exchange(false);
  const bool controlsChanged = SimulatorControls::takeRedrawRequest();
  if (!frameChanged && !controlsChanged) return;

  if (!texture || !sdl_renderer)
    return;

  extern GfxRenderer renderer;
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  applyWindowGeometryIfNeeded(orientation);
  const SimulatorControls::Layout layout = SimulatorControls::getLayout();

  {
    std::lock_guard<std::mutex> lock(pixelMutex);
    SDL_UpdateTexture(texture, nullptr, pixelBuf,
                      DISPLAY_WIDTH * sizeof(uint32_t));
  }
  SDL_SetRenderDrawColor(sdl_renderer, 0x15, 0x18, 0x1c, 0xff);
  SDL_RenderClear(sdl_renderer);

  // For portrait modes the landscape panel texture must be rotated to fill the
  // portrait window. SDL_RenderCopyEx rotates around the centre of dst, so dst
  // must stay landscape-oriented and be offset so its centre coincides with the
  // window centre. After rotation the result fills the portrait window.
  //
  // Portrait rotateCoordinates stores content rotated 90° CCW in the physical
  // buffer, so we rotate +90° CW here to undo it. PortraitInverted stores
  // content rotated 90° CW → undo with -90°.
  switch (orientation) {
  case GfxRenderer::Portrait: {
    // dst centre = window centre, landscape-sized panel texture.
    SDL_Rect dst = {layout.screen.x + (layout.screen.w - DISPLAY_WIDTH) / 2,
                    layout.screen.y + (layout.screen.h - DISPLAY_HEIGHT) / 2,
                    DISPLAY_WIDTH, DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::PortraitInverted: {
    SDL_Rect dst = {layout.screen.x + (layout.screen.w - DISPLAY_WIDTH) / 2,
                    layout.screen.y + (layout.screen.h - DISPLAY_HEIGHT) / 2,
                    DISPLAY_WIDTH, DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, -90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::LandscapeClockwise: {
    SDL_Rect dst = layout.screen;
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 180.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  default: {
    SDL_Rect dst = layout.screen;
    SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
    break;
  }
  }

  SDL_SetRenderDrawColor(sdl_renderer, 0x75, 0x7e, 0x87, 0xff);
  SDL_Rect screenBorder = {layout.screen.x - 1, layout.screen.y - 1,
                           layout.screen.w + 2, layout.screen.h + 2};
  SDL_RenderDrawRect(sdl_renderer, &screenBorder);
  SimulatorControls::draw(sdl_renderer, layout, DEVICE_NAME);
  SDL_RenderPresent(sdl_renderer);
  if (SimulatorControls::takeScreenshotRequest()) {
    saveScreenshot(orientation);
  }
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() { presentIfNeeded(); }

uint8_t *HalDisplay::getFrameBuffer() const {
  if (frameBufferLent) {
    return nullptr;
  }
  return frameBufferStorage.data();
}

uint8_t *HalDisplay::lendFrameBufferStorage(uint32_t *sizeOut) {
  if (sizeOut) {
    *sizeOut = frameBufferLent ? 0 : BUFFER_SIZE;
  }
  if (frameBufferLent) {
    return nullptr;
  }
  frameBufferLent = true;
  return frameBufferStorage.data();
}

void HalDisplay::returnFrameBufferStorage() {
  if (!frameBufferLent) {
    return;
  }
  frameBufferStorage.fill(0xFF);
  frameBufferLent = false;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t *lsbBuffer,
                                      const uint8_t *msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  displayBuffer(fallback, turnOffScreen);
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer) {
  std::lock_guard<std::mutex> lock(pixelMutex);
  copyPlane(grayscalePreviewState.lsbPlane, lsbBuffer,
            grayscalePreviewState.lsbValid);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msbBuffer) {
  std::lock_guard<std::mutex> lock(pixelMutex);
  copyPlane(grayscalePreviewState.msbPlane, msbBuffer,
            grayscalePreviewState.msbValid);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *bwBuffer) {
  std::lock_guard<std::mutex> lock(pixelMutex);
  if (bwBuffer) {
    snapshotBwBase(bwBuffer);
  } else {
    grayscalePreviewState.bwBaseValid = false;
    grayscalePreviewState.bwBase.fill(0);
    clearGrayscalePlanes();
  }
}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  std::lock_guard<std::mutex> lock(pixelMutex);
  composeGrayscalePreview();
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                          uint16_t yStart, uint16_t numRows) {
  if (!rows || numRows == 0 || yStart >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t rowsToCopy =
      (yStart + numRows > DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - yStart) : numRows;
  const size_t offset = static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES;
  const size_t byteCount =
      static_cast<size_t>(rowsToCopy) * DISPLAY_WIDTH_BYTES;
  std::lock_guard<std::mutex> lock(pixelMutex);
  auto &plane = lsbPlane ? grayscalePreviewState.lsbPlane
                         : grayscalePreviewState.msbPlane;
  memcpy(plane.data() + offset, rows, byteCount);
  if (lsbPlane) {
    grayscalePreviewState.lsbValid = true;
  } else {
    grayscalePreviewState.msbValid = true;
  }
}
bool HalDisplay::supportsStripGrayscale() const { return true; }

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return DISPLAY_WIDTH_BYTES;
}
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

HalDisplay display;
