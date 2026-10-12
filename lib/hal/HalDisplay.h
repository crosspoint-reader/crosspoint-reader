#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
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

  // Initialize the display hardware and driver.
  // When seamless=true, skip the on-wake resync so existing panel content is preserved
  // (used by Quick Resume to bring back the last reader page without a boot screen).
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
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);

  // Non-blocking display split — see EInkDisplay.h for full contract.
  // triggerDisplay() sends pixels + triggers waveform, swaps buffers, returns.
  // completeDisplay() sleeps (via FreeRTOS semaphore) until BUSY deasserts,
  // then does post-waveform SPI work. Both must be called from the render task.
  void triggerDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void completeDisplay();
  // X4 async refresh split: triggerDisplayAsync() returns while the waveform
  // runs (BUSY ISR armed); finishDisplayAsync() sleeps until it completes and
  // clears the refresh bookkeeping. Between the calls the caller may do
  // CPU/RAM-only work — no display or SPI-bus access, same task as trigger.
  // On X3 the trigger falls back to triggerDisplay() and finish is a no-op.
  void triggerDisplayAsync(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Whether triggerDisplayAsync() actually returns while the waveform runs on
  // this panel, rather than falling back to a blocking refresh. False on drivers
  // that cannot overlap (PanelDriver's default, e.g. LgfxEpd and Paper Mono) and
  // while an inversion is pending. Callers that spend the gap on real work --
  // the reader's inline AA -- must ask this rather than infer it from the board,
  // or they pay the plane/gray/restore cost with no overlap to hide it in.
  bool supportsAsyncRefresh() const;
  void finishDisplayAsync();
  bool isRefreshPending() const;
  bool isRedRamSynced() const;
  // Diagnostics: effective refresh mode of the last refresh (after any downgrade).
  RefreshMode getLastRefreshMode() const;
  // Measured duration of the last FAST refresh, 0 if none has run yet. See lastFastRefreshMs.
  uint16_t getLastFastRefreshMs() const { return lastFastRefreshMs; }
  // Seed the measurement from persisted storage so the first decision after a boot is as good as
  // the last one before it, rather than being made blind.
  void seedLastFastRefreshMs(uint16_t ms) {
    if (lastFastRefreshMs == 0) lastFastRefreshMs = ms;
  }
  // Diagnostics: last X4 displayMode byte (0x0C fast / 0x1C OTP-flash / 0xD4 half / 0x34 full).
  uint8_t getLastDisplayModeByte() const;

  // Request extra X3 ghost-clearing on the next display refresh.
  // No-op on non-X3 panels. Consumed by the next displayBuffer/refreshDisplay call.
  void requestResync(uint8_t settlePasses = 0);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Copy the just-displayed frame back into the write framebuffer. The display
  // path ends with a buffer swap, so the write buffer otherwise holds the frame
  // from two refreshes ago — partial repaints that patch a few regions and
  // re-display must call this first. No-op in single-buffer mode.
  void syncWriteBufferFromActive() const;

  // Release both frame buffers back to the heap (~52KB on X3, ~48KB on X4 each,
  // so ~104KB / ~96KB total). Call only after the final displayBuffer(); the
  // e-ink controller retains the image in its own RAM. No display operations
  // may be performed after this. The device must reboot before display resumes.
  void releaseBuffers();

  // Release only the secondary (previous-frame) buffer to free ~52 KB of heap
  // temporarily — e.g. during chapter compilation. BW rendering continues;
  // on X4 fast differential degrades to half/full until restored; on X3
  // fast differential is unaffected. Grayscale AA is unavailable until restored.
  // Returns true if the buffer was freed (false if already released).
  bool releaseSecondaryBuffer();

  // Restore the secondary buffer freed by releaseSecondaryBuffer().
  // Must be called before any grayscale AA pass or (on X4) fast differential.
  // Returns true on success, false on OOM.
  bool reallocSecondaryBuffer();

  // Returns true when the secondary (previous-frame) buffer is allocated.
  bool hasSecondaryBuffer() const;

  // Borrow/return variant of release/realloc: the display drops to the same
  // single-buffer mode, but the block stays owned by the display and is lent to
  // the caller as scratch — it never enters the heap, so nothing can allocate
  // inside it and returnSecondaryBuffer() cannot fail. See FreeInkDisplay.
  uint8_t* borrowSecondaryBuffer(size_t* size);
  bool returnSecondaryBuffer();

  // Allow fast differential refresh to continue (X4) after the secondary buffer
  // is released, diffing against the controller's retained RED-RAM baseline
  // instead of downgrading to half/full. See EInkDisplay::setSingleBufferFastDiff
  // for the contract the caller must uphold.
  void setSingleBufferFastDiff(bool enabled);

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void syncRedRamFromFrameBuffer();
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
  void cleanupGrayscaleWithPreviousBuffer();

  void displayGrayBuffer(bool turnOffScreen = false);

  // Ported from crosspoint-reader PR #3469 ("fix: vertical banding on X3
  // grayscale images", Bryan O'Sullivan / @bos). The diagnosis, the plane
  // encoding and the measurements are his: on a UC8279 X3 the checkerboard
  // column banding drops from 3.5-4.7% of the black-to-white range to 0.9%, and
  // the 8-gate-line period disappears. He also established that reshaping or
  // lengthening the nudge does not help -- only the long waveform does.
  //
  // The implementation differs. He added a supportsAbsoluteGrayPlanes() bool and
  // threaded a factoryMode flag through; our SDK already carries the generalized
  // GrayscaleMode / GrayscaleEncoding contract (Free-Ink/freeink-sdk#94 and #95,
  // also his, already merged into our submodule), so this selects
  // GrayscaleMode::Absolute instead.
  //
  // True when the panel accepts ABSOLUTE grayscale planes: planes that carry
  // every pixel, background included, instead of a differential mask layered
  // over the B/W base. The two encodings are not interchangeable — see
  // GfxRenderer::setAbsoluteGrayPlanes() for the bit patterns.
  //
  // On the X3 this selects the stock XTH4 waveform in place of the short AA
  // nudge. The nudge is what exposes the panel's 8-gate-line drive
  // nonuniformity as vertical banding on dithered grey images; the long,
  // DC-balanced XTH4 drive does not. It costs about a second per refresh
  // against the nudge's tenth, so it belongs on images, not on page turns.
  bool supportsAbsoluteGrayPlanes() const;

  // Open an absolute grayscale pass and push the framebuffer as its B/W base.
  // Returns false when the panel cannot run one, leaving the caller on the
  // ordinary differential flow — so callers need one branch, not two.
  //
  // Planes staged after this must cover the full panel height or the driver
  // abandons the pass; displayGrayBuffer() then runs the absolute waveform and
  // asks the controller for a clean B/W refresh next.
  bool beginAbsoluteGrayPass(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  // True when the panel can show a B/W base and its grayscale planes as ONE
  // waveform. Where it can, the two-push flow (base, then a grey overlay) is
  // not merely slower but wrong: a self-normalizing grey column expects the
  // pixel it drives not to have been driven already.
  bool supportsGrayFrame() const;
  // Compose the intact B/W framebuffer with the LSB/MSB planes staged by
  // copyGrayscale*Buffers() and display the result in one refresh. Falls back to
  // a plain displayBuffer() on a panel that cannot, so callers need no branch of
  // their own beyond deciding whether to stage planes at all.
  void displayGrayscaleFrame(RefreshMode refreshMode, bool turnOffScreen = false);
  // Deferred displayGrayscaleFrame(): returns while the waveform runs where the driver can, so
  // the caller can release its lock and let other work use the window. The caller owes a
  // completeDisplay() before the panel is touched again. Blocking fallback elsewhere.
  void triggerGrayscaleFrame(RefreshMode refreshMode, bool turnOffScreen = false);

  // Grey levels this panel resolves in one refresh. 4 on every dual-plane
  // controller (X3, X4, X4 Pro, M5 Paper Mono) — two selector bits per pixel is
  // the whole state space, so it is a hard ceiling, not a setting — and 16 on a
  // panel whose driver keeps a multi-bit buffer of its own (T5S3). Callers that
  // can render either way branch on this; everyone else keeps the plane path.
  uint8_t getGrayLevels() const;
  // Borrow the driver's composition buffer and paint 8-bit grey straight into
  // it: one byte per pixel (0x00 black, 0xFF white), `stride` bytes per row,
  // getDisplayHeight() rows, PHYSICAL panel orientation. Costs no heap — the
  // buffer is the driver's own and already allocated — and returns nullptr
  // wherever that is not on offer, which is every 4-level panel.
  //
  // The loan ends at displayGray8Canvas() or at the next ordinary refresh; a
  // caller that borrows and then abandons the frame corrupts nothing, because
  // every normal push rebuilds the buffer from the 1-bpp framebuffer.
  uint8_t* borrowGray8Canvas(uint16_t* stride);
  // Quantise the borrowed canvas to the panel's native depth and show it as one
  // waveform. The driver picks a bank that can land every level, so this may
  // refresh more thoroughly (and more slowly) than `refreshMode` asked for.
  void displayGray8Canvas(RefreshMode refreshMode, bool turnOffScreen = false);

  // What the panel does, from the SDK (FreeInkDisplay): ask these rather than which board this
  // is. They are what the old deviceIsX3() callers meant, and an X4 Pro can come up on any of
  // three controllers.
  //
  // A FAST refresh diffs against the previous frame the host keeps (the secondary buffer, or
  // RED seeded from it), so lending or releasing the secondary costs FAST refreshes unless RED
  // is seeded first. False on the X3, whose controller keeps its own baseline (DTM1).
  bool fastRefreshNeedsHostFrame() const;
  // An async refresh keeps the live frame buffer in use, and the refresh pending, until it
  // completes -- for the whole waveform (the X3).
  bool asyncRefreshHoldsFrameBuffer() const;
  // Repeated FAST refreshes leave residue only a HALF clears (the X3's UC8253 / UC8279d).
  bool ghostsUnderFastRefresh() const;

  // X3-only knob: pick between the OEM 53-frame grayscale LUT (default, slow
  // and accurate) and the 7-frame community LUT (fast, slightly darker
  // mid-tones). No effect on X4. See EInkDisplay::setFastGrayscaleLut.
  void setFastGrayscaleLut(bool fast);
  bool getFastGrayscaleLut() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
  uint8_t pendingX3SettlePasses = 0;
  RefreshMode lastRefreshMode = RefreshMode::FAST_REFRESH;

  // How long this panel's last FAST refresh actually took, in ms. Measured rather than derived
  // from the board, because it is a property of the controller the runtime probe selected, not of
  // the model name -- an X4 Pro can come up on SSD1677, UC8179 or UC8279, and they do not agree.
  //
  // FAST specifically: it is the mode every intermediate repaint uses, so it is what a caller
  // deciding whether an extra repaint is affordable needs to know. 0 means "not measured yet".
  uint16_t lastFastRefreshMs = 0;
  uint8_t lastDisplayModeByte = 0x0C;  // default to fast refresh mode byte
};

extern HalDisplay display;
