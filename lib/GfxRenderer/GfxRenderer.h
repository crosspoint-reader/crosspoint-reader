#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode {
    BW,             // 1-bit black/white
    GRAYSCALE_LSB,  // Differential gray: mark pixels for LSB plane (clearScreen(0x00) + drawPixel(false))
    GRAYSCALE_MSB,  // Differential gray: mark pixels for MSB plane (clearScreen(0x00) + drawPixel(false))
    GRAY2_LSB,      // Factory absolute gray: encode BW RAM = bit0 (clearScreen(0x00) + drawPixel(false))
    GRAY2_MSB,      // Factory absolute gray: encode RED RAM = bit1 (clearScreen(0x00) + drawPixel(false))
  };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

  // Selects LUT and pixel-plane encoding for renderGrayscale() / renderGrayscaleSinglePass() /
  // displayXtchPlanes(). Pre-flash policy is per-call, not part of the drive mode.
  enum class GrayscaleDriveMode {
    FactoryFast,     // Factory absolute 2-bit (lut_factory_fast)
    FactoryQuality,  // Factory absolute 2-bit (lut_factory_quality)
    Differential,    // Differential 2-bit overlay (no LUT); requires prior BW state
  };

  // Display state — tracks whether the physical display was last updated via a factory LUT render.
  // BW: frameBuffer mirrors the display (menus, EPUB reader).
  // FactoryLut: display holds a grayscale image; frameBuffer has been reset to white by
  // cleanupGrayscaleWithFrameBuffer() and no longer represents what is visually shown.
  enum class DisplayState { BW, FactoryLut };

  // One-shot hook fired in renderGrayscaleSinglePass after renderFn() writes both planes
  // but before they are pushed to the controller. At that moment:
  //   lsbPlane = frameBuffer (LSB / BW RAM plane)
  //   msbPlane = secondaryFrameBuffer (MSB / RED RAM plane)
  // Hook is cleared automatically after firing.
  using ScreenshotHook = void (*)(const uint8_t* lsbPlane, const uint8_t* msbPlane, int physWidth, int physHeight,
                                  void* ctx);
  using RenderHook = void (*)(const GfxRenderer&, const void*);

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint8_t* secondaryFrameBuffer = nullptr;  // MSB plane buffer for single-pass grayscale decode
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  mutable DisplayState displayState = DisplayState::BW;
  ScreenshotHook screenshotHook = nullptr;
  void* screenshotHookCtx = nullptr;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  void drawPixelToBuffer(uint8_t* buf, int x, int y) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  DisplayState getDisplayState() const { return displayState; }
  void setScreenshotHook(ScreenshotHook hook, void* ctx);

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(const unsigned char* lut = nullptr, bool factoryMode = false) const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;
  // Two-pass grayscale render. renderFn is called twice: once with the LSB render mode set
  // (writes BW RAM plane), then with the MSB mode set (writes RED RAM plane). The method
  // handles pre-flash (FactoryFast only), clearScreen, setRenderMode, buffer copies,
  // displayGrayBuffer, and resets renderMode to BW on completion.
  // storeBwBuffer / restoreBwBuffer remain the caller's responsibility.
  // preFlashOverlayFn (optional): called after clearScreen() but before the pre-flash displayBuffer(),
  // allowing callers to draw a loading indicator that appears during the pre-flash without an extra refresh.
  void renderGrayscale(GrayscaleDriveMode mode, void (*renderFn)(const GfxRenderer&, const void*), const void* ctx,
                       void (*preFlashOverlayFn)(const GfxRenderer&, const void*) = nullptr,
                       const void* preFlashCtx = nullptr);
  // Single-pass variant: calls renderFn once in GRAY2_LSB mode while simultaneously writing
  // the MSB plane to a secondary buffer. Cuts SD card reads from 2 to 1 for file-backed renders.
  // Falls back to two-pass on secondary buffer allocation failure. Factory modes pre-flash to
  // white with HALF_REFRESH by default. preFlashPasses=0 skips the internal pre-flash so
  // callers can run a custom conditioning sequence first.
  void renderGrayscaleSinglePass(GrayscaleDriveMode mode, void (*renderFn)(const GfxRenderer&, const void*),
                                 const void* ctx, void (*preFlashOverlayFn)(const GfxRenderer&, const void*) = nullptr,
                                 const void* preFlashCtx = nullptr,
                                 HalDisplay::RefreshMode preFlashRefreshMode = HalDisplay::HALF_REFRESH,
                                 uint8_t preFlashPasses = 1);

  // Direct 2-bit XTCH plane blit using factory LUT. Caller supplies the two decoded bit planes
  // (plane1 = BW RAM / LSB, plane2 = RED RAM / MSB) in column-major order matching XTCH encoding.
  // Handles optional pre-flash, both RAM writes, factory LUT fire, and BW controller sync internally.
  // mode selects the factory LUT: FactoryFast (default) or FactoryQuality; Differential is invalid here.
  void displayXtchPlanes(const uint8_t* plane1, const uint8_t* plane2, uint16_t pageWidth, uint16_t pageHeight,
                         RenderHook overlayFn = nullptr, const void* overlayCtx = nullptr,
                         GrayscaleDriveMode mode = GrayscaleDriveMode::FactoryFast, bool preFlash = false,
                         HalDisplay::RefreshMode preFlashRefreshMode = HalDisplay::HALF_REFRESH,
                         uint8_t preFlashPasses = 1);

  // 1-bit XTC page via the same grayscale LUT pipeline. Row-major pageBuffer (XTC: 0=black, 1=white).
  // BW and RED RAM receive identical data since there are no intermediate gray levels.
  void displayXtcBwPage(const uint8_t* pageBuffer, uint16_t pageWidth, uint16_t pageHeight,
                        HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH, RenderHook overlayFn = nullptr,
                        const void* overlayCtx = nullptr);

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Low level functions
  uint8_t* getFrameBuffer() const;
  uint8_t* getSecondaryFrameBuffer() const { return secondaryFrameBuffer; }
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }
};
