#pragma once
#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>

// Gray UI: screens that ask for it are drawn as a 16-level image, with
// anti-aliased text, icons and corners and flat grays, on panels that take
// one. Compiled only where the board has one and the SDK provides the call.
#ifndef CROSSPOINT_GRAY_UI
#if defined(FREEINK_HAS_GRAY16) && FREEINK_DEVICE_METALIO_EINK4_PLUS
#define CROSSPOINT_GRAY_UI 1
#else
#define CROSSPOINT_GRAY_UI 0
#endif
#endif

// What a gray UI frame carries. 0 (the default): only two edge levels, on
// glyph, icon and corner edges, with dithered fills and 1-bit covers. This is
// the mode that has been judged on the Metalio E-Ink4-Plus. 1: all 16 levels,
// with flat gray fills and gray cover thumbnails. Its table gives an even,
// smooth ramp, but a smooth gray is reached by way of black, and with whole
// areas doing that the screen changes looked bad on the device.
#ifndef CROSSPOINT_GRAY_UI_FULL_TONE
#define CROSSPOINT_GRAY_UI_FULL_TONE 0
#endif
#if !CROSSPOINT_GRAY_UI
#undef CROSSPOINT_GRAY_UI_FULL_TONE
#define CROSSPOINT_GRAY_UI_FULL_TONE 0
#endif

// Hi-res UI icons: a board with a UI scale draws an icon from artwork made at
// twice its drawn size instead of enlarging the 1x icon. Compiled only for
// the board that ships that artwork (src/components/icons/hiResIcons.h).
#ifndef CROSSPOINT_HIRES_UI_ICONS
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#define CROSSPOINT_HIRES_UI_ICONS 1
#else
#define CROSSPOINT_HIRES_UI_ICONS 0
#endif
#endif

// Serial commands for calibrating 16-level drawing on the glass
// (src/util/Gray16Tuning.h). A development tool: off unless the build asks.
#ifndef CROSSPOINT_GRAY16_TUNING
#define CROSSPOINT_GRAY16_TUNING 0
#endif
#if !CROSSPOINT_GRAY_UI
#undef CROSSPOINT_GRAY16_TUNING
#define CROSSPOINT_GRAY16_TUNING 0
#endif

class HalDisplay {
 public:
  using Controller = BoardConfig::DisplayController;
  Controller getController() const;

  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;

  // When checking stripUploads, pass the grayscale mode used for the planes.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const;
  // Controller of the driver chosen at boot (null if the driver does not say).
  const char* controllerName() const { return einkDisplay.getControllerName(); }
  // Part number of the panel the boot probe identified (null if unknown).
  const char* panelName() const { return einkDisplay.getPanelName(); }
  // True while book pages are presented; panels with a reading waveform use it.
  void setReaderRefresh(bool reading) { einkDisplay.setReaderRefresh(reading); }
  // True asks text-overlay gray passes for every gray level (image pages).
  void setOverlayFullTone(bool fullTone) { einkDisplay.setOverlayFullTone(fullTone); }
  bool displayRetainedGrayscaleOverlay(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool restore) {
    return einkDisplay.displayRetainedGrayscaleOverlay(x, y, w, h, restore);
  }

  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  // Non-blocking refresh (shadow-free): starts the panel waveform and returns
  // while the panel refreshes on its own. The framebuffer must stay untouched
  // until waitRefreshComplete(), and the caller must rebuild the differential
  // baseline before the next differential update (the tiled grayscale cleanup
  // does). Panels without deferral fall back to a blocking refresh.
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  // Block until a pending deferred refresh completes (no-op when none is).
  void waitRefreshComplete();
  // True when displayBufferAsync() genuinely overlaps (panel driver defers);
  // false where it falls back to a blocking refresh.
  bool supportsAsyncRefresh() const;

  // Output polarity. The framebuffer remains in normal polarity; inversion is
  // applied by the display driver while sending it to the panel.
  void setInverted(bool inverted);
  bool isInverted() const;

  // Power management
  void deepSleep();
  // Power the panel down between frames, keeping its image; the next paint wakes it.
  void controllerIdle();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Lend the framebuffer's ~48 KB STORAGE to a memory-hungry phase (chapter
  // builds) without freeing it: the allocation never moves, so repeated loans
  // cannot fragment the heap (free+realloc measurably did). No display calls
  // between lend and return; the panel keeps its last refreshed image. The
  // buffer comes back white — redraw fully. Returns nullptr if already lent.
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();

  // X3 grayscale preconditioning (OEM "AA-pre-BW(mid)" settle pass), windowed
  // to the gray region in physical panel coordinates (no-arg = full frame).
  // Call after the BW base frame is displayed and before the grayscale planes
  // are written; no-op on X4. See EInkDisplay::preconditionGrayscale.
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. On X3, HALF fallback first requests a resync to match
  // displayBuffer(HALF); FAST fallback keeps the OEM differential base waveform
  // ("AA-pre-BW(mid)"). Other panels display normally with `fallback` mode.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  bool displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

  // Tiled grayscale: stream one band of a plane (lsbPlane selects LSB/MSB RAM)
  // straight to the controller. Check grayscaleCapabilities(mode).stripUploads
  // with the same grayscale mode used for the planes.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);

#if CROSSPOINT_GRAY_UI
  // 16-level image: 4 bpp, 0 = white .. 15 = darkest, left pixel in the high
  // nibble, getDisplayWidth() / 2 bytes per panel row. fast drives only the
  // pixels that change.
  bool supportsGray16Image() const { return einkDisplay.supportsGray16Image(); }
  bool displayGray16Image(const uint8_t* levels, bool fast) {
    return einkDisplay.displayGray16Image(levels, true, fast);
  }
#endif
#if CROSSPOINT_GRAY_UI
  // Three numbers per level (0 = white .. 15 = darkest) that say how the panel
  // drives it; nullptr returns to the built-in levels. See the SDK's driver
  // for their meaning.
  bool setGray16Levels(const uint8_t (*paths)[3]) { return einkDisplay.setGray16Levels(paths); }
  bool getGray16Levels(uint8_t (*paths)[3]) const { return einkDisplay.getGray16Levels(paths); }
#endif

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
};

extern HalDisplay display;
