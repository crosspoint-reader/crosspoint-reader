#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Bitmap.h"
#include "FontLookup.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory
  static constexpr unsigned int REFRESH_OVERRIDE_NONE = 0;

  HalDisplay& display;
  std::atomic<int> renderMode;
  // Selects the absolute plane encoding for the grayscale passes; see
  // setAbsoluteGrayPlanes(). Cleared whenever the mode returns to BW.
  std::atomic<bool> absoluteGrayPlanes;
  std::atomic<int> orientation;
  // Mirrors `orientation` except across the themes' transient hint-strip flips. See setOrientation.
  std::atomic<int> heldOrientation;
  std::atomic<bool> fadingFix;
  // Extra pixels inset from every panel edge, on top of the board profile's viewableInsets.
  // See setViewablePadding().
  std::atomic<int> viewablePadding;
  // Text darkness for 2-bit grayscale glyph rendering.
  std::atomic<uint8_t> textDarkness;
  //   0 = Normal     — true 4-level AA (raw=1 → light gray, raw=2 → dark gray)
  //   1 = Dark       — historical default; raw=2 collapses to black
  //   2 = Extra Dark — both AA shades go black in the grayscale plane
  //   3 = Maximum    — grayscale pass skipped entirely; only the hard BW
  //                    pass remains, so AA pixels render as solid black
  //                    with the FAST waveform (no gray-LUT softening)
  // Only affects AA pixels in GRAYSCALE_MSB / GRAYSCALE_LSB rendering of 2-bit fonts.
  // 1-bit fonts and the BW pass are unchanged. Default is 1 to preserve historical
  // rendering. See drawMaskFor2BitMode() in GfxRenderer.cpp for the per-level
  // pixel breakdown and a worked example glyph.
  mutable uint8_t* frameBuffer = nullptr;
  mutable bool cacheOnlyImageWrites_ = false;  // see ScopedCacheOnlyImageWrites
  uint16_t panelWidth = 0;                     // set in begin()
  uint16_t panelHeight = 0;                    // set in begin()
  uint16_t panelWidthBytes = 0;                // set in begin()
  uint32_t frameBufferSize = 0;                // set in begin()
  uint16_t bwSnapshotRowStart = 0;
  uint16_t bwSnapshotRowEnd = 0;
  size_t bwSnapshotSizeBytes = 0;
  size_t bwBufferChunkSize = BW_BUFFER_CHUNK_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  // Mutable because a lookup from a const draw or measure call may register a font the resolver
  // supplies on demand (findFontFamily). Same compromise as sdCardFonts_ below.
  mutable std::map<int, EpdFontFamily> fontMap;
  FontResolverHook<EpdFontFamily> fontResolver_;
  // The one place a font ID becomes a family: fontMap, then the resolver for an unknown ID.
  const EpdFontFamily* findFontFamily(const int fontId) const {
    return findOrResolveFont(fontMap, fontId, fontResolver_);
  }
  // Font IDs that render another family's faces at a fixed scale -- see insertScaledFont(). Only
  // synthesised sizes appear here, so the map is empty on a build that ships every size for real
  // and the lookup is a miss that costs one comparison.
  std::map<int, float> fontBaseScales;
  // Mutable because ensureFontReady() is const (called from layout code that
  // holds a const GfxRenderer&) but triggers SD card reads and heap allocation
  // inside the SdCardFont objects. Same pragmatic compromise as fontCacheManager_.
  mutable std::map<int, SdCardFont*> sdCardFonts_;
  // Scaled alias IDs (SdCardFontManager::ensureSizeAlias) and the SdCardFont behind each. Kept
  // apart from sdCardFonts_ on purpose: the per-font maintenance loops (metadata drop/restore,
  // cache clear, stats) walk sdCardFonts_ and must visit a font ONCE, while the per-ID lookups
  // (ensureFontReady, the prewarm scan) go through sdCardFontFor() and see both.
  mutable std::map<int, SdCardFont*> sdCardFontAliases_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // --- Scaled-glyph mask cache ----------------------------------------------
  // Text drawn at a scale != 1.0 — i.e. every word of any book whose stylesheet
  // puts a font-size on body text, which is most of them — is resampled per
  // destination pixel by renderCharAtScale(). Uncached, that resample runs once
  // per glyph *occurrence*: ~500 times a page against ~40 distinct glyphs.
  // Measured 2026-08-02 (X4, "A Scandalous Deception", body text at 0.92em):
  // 303 ms for the page's BW pass with the scaling, 39 ms with it off.
  //
  // Caching the resampled 1-bit coverage mask makes the resample once per
  // distinct (glyph, scale, selector) and lets renderGlyphFastBW() — the same
  // 8-pixel-chunk blitter unscaled text uses — draw every occurrence.
  //
  // Budget: 80 x 16B table + 3.5 KB arena ≈ 4.75 KB, two allocations taken at
  // reader entry (ensureScaledGlyphCache) and given back at reader exit
  // (releaseGlyphCaches). It is
  // a bump allocator that resets wholesale when full: no per-glyph allocation,
  // nothing to fragment, and a page whose working set overflows the arena
  // degrades toward the uncached path instead of growing.
  //
  // Not internally synchronised: it is written only from glyph rasterisation,
  // which every caller (render task, loop-task pre-render, deferred AA) already
  // serialises behind RenderLock — the same lock that protects the framebuffer
  // these masks are blitted into.
  //
  // Keyed by (font, codepoint, scale, sel), NOT by EpdGlyph address: SD-card
  // fonts rebuild their glyph array every page and serve overflow codepoints
  // from an 8-entry ring, so the same address means different glyphs over time.
  // The resampled pixels are a pure function of this key, which makes it both
  // stable and exact. Style is deliberately absent: EpdFontFamily::getData() and
  // getGlyph() both resolve through getFont(style), so the EpdFontData pointer
  // already identifies the variant.
  struct ScaledGlyphEntry {
    const void* fontData;  // EpdFontData for the style (stable per font object)
    uint32_t scaleBits;    // exact float bits of the scale factor (no quantisation)
    uint16_t cp;           // codepoint; above the BMP renders uncached (see alloc)
    uint16_t offset;       // byte offset into scaledGlyphArena_
    uint8_t sel;           // minRaw2Bit (scale < 1) or drawMask (scale >= 1)
    uint8_t w;
    uint8_t h;
  };
  // Keeps the documented budget honest if a field is ever added.
  static_assert(sizeof(ScaledGlyphEntry) <= 16, "scaled-glyph entry grew; update the cache budget comment");
  // Sized from measurement, not guesswork: a page of this corpus needs 45-58
  // distinct renditions (three font groups x ~40 glyphs, per the prewarm logs)
  // averaging ~40 bytes of mask each. 48 entries reset on every pass — the table,
  // not the arena, was the binding constraint.
  static constexpr uint8_t SCALED_GLYPH_MAX_ENTRIES = 80;
  static constexpr uint16_t SCALED_GLYPH_ARENA_BYTES = 3584;
  // What the arena has to hold once a BODY size is synthesised (see insertScaledFont): every
  // glyph on the page goes through the resampler, at the 20 pt master's glyph size times the
  // base scale. Measured over a realistic 62-glyph page: 7,820 B at 26 pt Bookerly, 6,659 B at
  // 24 pt -- 1.9-2.2x the arena above, which was sized for CSS-scaled body text with ~40 B
  // masks. Overflowing it resets the cache mid-page, so the working set resamples two or three
  // times per page instead of once, and at ~1 ms per glyph at these sizes that is 60-120 ms a
  // page turn on exactly the sizes added for readers who need them most.
  //
  // Chosen only when the body text itself is a synthesised size (ensureScaledGlyphCache's
  // bodyFontId), so a reader at a real size keeps the smaller block. It used to follow "is any
  // scaled font registered" -- which main.cpp always does for 22/24/26 pt, so every reader paid
  // the +4,608 B, permanently, whatever size they read at.
  static constexpr uint16_t SCALED_GLYPH_ARENA_BYTES_SYNTH = 8192;
  mutable uint16_t scaledGlyphArenaBytes_ = SCALED_GLYPH_ARENA_BYTES;  // actual size once allocated
  // One outsized glyph (a large heading) must not evict a page's whole body-text
  // working set, so anything above this renders uncached.
  static constexpr uint16_t SCALED_GLYPH_MAX_MASK_BYTES = 512;
  mutable std::unique_ptr<ScaledGlyphEntry[]> scaledGlyphEntries_;
  mutable std::unique_ptr<uint8_t[]> scaledGlyphArena_;
  mutable uint8_t scaledGlyphCount_ = 0;
  mutable uint16_t scaledGlyphUsed_ = 0;
  mutable bool scaledGlyphOom_ = false;  // arena allocation failed; do not retry every glyph

 public:
  // Allocate the scaled-glyph cache now rather than on first scaled glyph. Call at a stable
  // point (reader entry); see the definition for why the timing matters more than the size.
  // `bodyFontId` is the font the reader is about to lay its text out in: the larger arena only
  // when that size is synthesised (insertScaledFont), and a cache of the other size from the last
  // book is replaced here. 0 (unknown) keeps whatever is there, or takes the smaller one.
  bool ensureScaledGlyphCache(int bodyFontId = 0) const;

 private:
  mutable std::atomic<unsigned int> refreshOverride = REFRESH_OVERRIDE_NONE;
  // Atomically consume a pending setNextDisplayRefreshMode() override: if one is set, clear it
  // and return its mode; otherwise return `requested`. Shared by displayBuffer() and
  // triggerDisplay() so the override is honored on BOTH the blocking and non-blocking display
  // paths (otherwise a HALF set before a triggerDisplay() render would persist and leak onto a
  // later displayBuffer() turn).
  HalDisplay::RefreshMode consumeRefreshOverride(HalDisplay::RefreshMode requested) const;

  // FAST refreshes shipped since the last HALF or FULL. Saturating uint8: the only question ever
  // asked of it is "at least N?", and N is single digits. Starts saturated so a repaint issued
  // before any refresh has happened still gets its ghost-clearing HALF.
  mutable uint8_t fastRefreshesSinceFull_ = 0xFF;
  // Counts one shipped frame. Called from every path that reaches the panel, with the EFFECTIVE
  // mode (i.e. after consumeRefreshOverride), because an armed override is what actually drives
  // the waveform and therefore what actually clears the ghosting.
  void noteRefresh(HalDisplay::RefreshMode effectiveMode) const {
    if (effectiveMode == HalDisplay::FAST_REFRESH) {
      if (fastRefreshesSinceFull_ < 0xFF) fastRefreshesSinceFull_++;
    } else {
      fastRefreshesSinceFull_ = 0;
    }
  }

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Write a patterned horizontal span directly to the physical framebuffer using byte-level operations.
  // phyY: physical row; phyX_start/phyX_end: inclusive physical column range.
  // patternByte is repeated across the span; partial edge bytes are blended with existing content.
  // Bit layout: MSB-first (bit 7 = phyX=0); 0 bits = dark pixel, 1 bits = white pixel.
  void fillPhysicalHSpanByte(int phyY, int phyX_start, int phyX_end, uint8_t patternByte) const;
  // Write a solid horizontal span directly to the physical framebuffer using byte-level operations.
  // Thin wrapper around fillPhysicalHSpanByte: state=true → 0x00 (dark), false → 0xFF (white).
  void fillPhysicalHSpan(int phyY, int phyX_start, int phyX_end, bool state) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay),
        renderMode(static_cast<int>(BW)),
        absoluteGrayPlanes(false),
        orientation(static_cast<int>(Portrait)),
        heldOrientation(static_cast<int>(Portrait)),
        fadingFix(false),
        viewablePadding(0),
        textDarkness(1) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Bezel insets live in BoardProfile::viewableInsets and are read by
  // getOrientedViewableTRBL(). The constants that used to sit here duplicated the
  // profile's own defaults, which is how every board ended up with the X4's bezel
  // geometry; keeping them would leave two sources of truth for one measurement.

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Rebinds an already-registered ID to a different family, where insertFont() deliberately
  // refuses a duplicate. Used by the UI font ladder: the three UI font IDs are LOGICAL slots
  // whose family depends on SETTINGS.uiFontSize, so switching the setting rebinds them in
  // place rather than teaching ~600 call sites about a second set of IDs. Registers the ID if
  // it is not bound yet, so the first bind and every later one go through one call.
  void replaceFont(int fontId, EpdFontFamily font) {
    fontMap.insert_or_assign(fontId, font);
    // A rebound ID is a REAL face again. Leaving a base scale behind would keep scaling it.
    fontBaseScales.erase(fontId);
    invalidateScaledGlyphCache();
  }
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    fontBaseScales.erase(fontId);
    invalidateScaledGlyphCache();
  }

  /// Registers a font ID that has NO faces of its own: it renders `font` scaled by `scale`.
  ///
  /// This is how the reader offers sizes above its largest real face — 22/24/26 pt come off the
  /// 20 pt master at x1.10, x1.20 and x1.30. Shipping them as real faces costs ~1.9 MB the app
  /// partition does not have; scaling costs nothing but the resample, which measures ~1.4 us per
  /// glyph and is cached per distinct glyph per page (see ScaledGlyphEntry).
  ///
  /// Why the scale lives HERE rather than being passed by the reader: a distinct font ID is
  /// already the unit everything downstream keys on. Layout asks getTextWidth(id), pagination
  /// hashes the id into the section cache property block, and the on-disk section header stores
  /// it — so a synthesised size caches separately from its master with no extra plumbing, and
  /// ~600 existing call sites keep working unchanged. Passing the scale through the reader
  /// instead would mean touching every one of them and the cache key besides.
  ///
  /// The base scale COMPOUNDS with a per-block CSS scale rather than replacing it: a heading at
  /// 1.6em inside 24 pt body text resolves to one resample at 1.2 x 1.6, not two.
  ///
  /// NOT for UI font IDs. FreeInkUI (freeink-sdk) centres icons by reading a glyph's raw width,
  /// left and top straight out of getFontMap(), which for a synthesised ID are the master's and
  /// would place every icon wrong. Reader sizes only; the UI ladder rebinds real faces.
  void insertScaledFont(int fontId, EpdFontFamily font, float scale) {
    fontMap.insert_or_assign(fontId, font);
    if (scale > 0.99f && scale < 1.01f) {
      fontBaseScales.erase(fontId);  // a scale of 1 is a real face; do not pay for the lookup
    } else {
      fontBaseScales.insert_or_assign(fontId, scale);
    }
    invalidateScaledGlyphCache();
  }

  /// The scale `fontId` renders its master at, or exactly 1.0f for a font with real faces.
  ///
  /// Callers that need a size in pixels should use the ordinary accessors, which already apply
  /// this; it is exposed for the few places that must know a size is synthesised at all.
  float fontBaseScale(int fontId) const {
    const auto it = fontBaseScales.find(fontId);
    return it == fontBaseScales.end() ? 1.0f : it->second;
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  // Drop every rebuildable font cache (glyph metadata, bitmaps, kern) while leaving
  // the fonts loaded and usable — everything faults back in on demand. Lets callers
  // reclaim that space without taking a dependency on FontCacheManager themselves.
  // Returns false when there is no cache manager to ask.
  bool releaseFontCaches();
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  /// Lets a font source supply families for IDs nobody registered yet, on first use. The
  /// resolver is asked on every lookup of an ID that is not in fontMap, so declining must be
  /// cheap. A family it returns is emplaced into fontMap directly (no insertFont(): no cache
  /// invalidation, no log). It must not look fonts up through this renderer, or it recurses.
  void setFontResolver(std::optional<EpdFontFamily> (*resolve)(void* ctx, int fontId), void* ctx) {
    fontResolver_ = FontResolverHook<EpdFontFamily>{resolve, ctx};
  }
  // Each of these can retire an EpdFontData a cached scaled mask is keyed on, so
  // they all drop the cache (see ScaledGlyphEntry).
  void registerSdCardFont(int fontId, SdCardFont* font) {
    sdCardFonts_[fontId] = font;
    invalidateScaledGlyphCache();
  }
  // A second ID served by an already-registered SdCardFont, at a base scale set separately via
  // insertScaledFont(). See the note on sdCardFontAliases_ for why this is not registerSdCardFont().
  void registerSdCardFontAlias(int fontId, SdCardFont* font) {
    sdCardFontAliases_[fontId] = font;
    invalidateScaledGlyphCache();
  }
  void unregisterSdCardFont(int fontId) {
    sdCardFonts_.erase(fontId);
    sdCardFontAliases_.erase(fontId);
    invalidateScaledGlyphCache();
  }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    sdCardFontAliases_.clear();
    invalidateScaledGlyphCache();
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  const std::map<int, SdCardFont*>& getSdCardFontAliases() const { return sdCardFontAliases_; }
  bool isSdCardFont(int fontId) const { return sdCardFontFor(fontId) != nullptr; }
  // The SdCardFont serving fontId, whether native or alias; nullptr for a built-in font.
  SdCardFont* sdCardFontFor(int fontId) const {
    auto it = sdCardFonts_.find(fontId);
    if (it != sdCardFonts_.end()) return it->second;
    it = sdCardFontAliases_.find(fontId);
    return it != sdCardFontAliases_.end() ? it->second : nullptr;
  }

  // Ensure glyph metrics are loaded for the given text before layout measurement.
  // No-op for built-in fonts (map lookup finds nothing and returns immediately).
  // For SD/flash fonts: reads glyph metrics (no bitmaps) for all codepoints in text.
  void ensureFontReady(int fontId, const char* utf8Text) const;

  // Clear the cumulative font metadata cache built up across paragraphs.
  // No-op when no SD font is active.
  void clearFontAccumulation() const;

  // Phase lifecycle: drop layout-phase metadata to free heap before createSectionFile().
  // No-op when no SD font is active or font is mmap'd (metadata is always accessible).
  void dropFontMetadata() const;

  // Restore layout-phase metadata after createSectionFile().
  // Returns true if all fonts reloaded successfully (always true for mmap fonts).
  bool restoreFontMetadata() const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) {
    orientation.store(static_cast<int>(o), std::memory_order_relaxed);
    heldOrientation.store(static_cast<int>(o), std::memory_order_relaxed);
  }
  Orientation getOrientation() const { return static_cast<Orientation>(orientation.load(std::memory_order_relaxed)); }

  // Draw in `o` WITHOUT changing what getHeldOrientation() reports. For furniture that has to sit
  // on a fixed panel edge whichever way the device is held — the button-hint strips flip to
  // Portrait so their boxes land beside the physical buttons, then flip back.
  void setDrawOrientation(const Orientation o) { orientation.store(static_cast<int>(o), std::memory_order_relaxed); }
  // The orientation the reader is actually holding the device in, unaffected by the transient
  // flips above. Render runs on its own task, so anything sampling orientation from OUTSIDE a
  // render pass — the input layer's logical directions above all — must read this instead of
  // getOrientation(), or it will now and then catch a hint strip mid-draw and answer Portrait.
  Orientation getHeldOrientation() const {
    return static_cast<Orientation>(heldOrientation.load(std::memory_order_relaxed));
  }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix.store(enabled, std::memory_order_relaxed); }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  // Logical screen size in an EXPLICIT orientation, for a caller that has already
  // sampled one and must not sample it again. The no-argument versions read the
  // LIVE draw orientation, which the themes flip to Portrait mid-pass for the
  // hint strips — so a caller on the loop task that reads width, then height,
  // then maps a touch, can catch a different frame in each of the three. Read
  // getHeldOrientation() once and pass it here instead.
  int getScreenWidth(Orientation orientation) const;
  int getScreenHeight(Orientation orientation) const;
  // Map a touch point from normalized PANEL-NATIVE coordinates (0..1, the frame
  // InputManager reports in, per the BoardConfig touch contract) to LOGICAL
  // screen pixels under the renderer's live orientation.
  //
  // The renderer is the only authority on the orientation actually on screen —
  // the reader rotates it and restores portrait on exit — so the touch
  // transform is read from it rather than from the persisted setting. This is
  // the same discipline setStripReversedPredicate already applies to the front
  // buttons, and it is why MappedInputManager holds a renderer reference.
  //
  // Output is clamped to the panel, so a caller always gets an on-screen point.
  // Ported verbatim from upstream/develop; see
  // docs/contributing/touch-architecture.md, "Coordinate transform".
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  // Same, but into an EXPLICIT orientation's frame rather than the live one. For geometry
  // that was drawn in a fixed frame regardless of how the screen is rotated -- the button
  // hint strip, which forces Portrait for the duration of its draw. Asking for the tap in
  // the frame the boxes were recorded in is what makes the hit test orientation-independent
  // instead of only correct while the screen happens to be portrait.
  void tapToLogical(Orientation orientation, float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void setNextDisplayRefreshMode(HalDisplay::RefreshMode refreshMode) const;
  // True if a setNextDisplayRefreshMode() override is armed but not yet consumed. Peek only —
  // does NOT consume it (unlike consumeRefreshOverride). Lets a caller that is about to issue an
  // intermediate refresh (which would consume the override) detect that a deliberate-transition
  // override is pending and react accordingly.
  bool hasRefreshOverridePending() const {
    return refreshOverride.load(std::memory_order_acquire) != REFRESH_OVERRIDE_NONE;
  }
  // Discard a pending setNextDisplayRefreshMode() override without applying it, so the next display
  // uses its own requested mode. Use when the armed override should move to a later refresh — e.g.
  // the reader keeps the indexing popup FAST but forces the following content page to HALF itself.
  void clearRefreshOverride() const { consumeRefreshOverride(HalDisplay::FAST_REFRESH); }

  // FAST refreshes a non-reader screen may accumulate before another ghost-clearing HALF earns
  // its cost. Deliberately far below the reader's refreshFrequencyPages (15): a page turn
  // replaces every word on the panel where a menu repaint changes a highlight bar and a few
  // rows, so UI ghosting builds more slowly -- but UI text is also read from close up, so this
  // stays conservative rather than maximal.
  static constexpr uint8_t GHOST_CLEARING_FAST_BUDGET = 4;
  // True when enough FAST refreshes have accumulated since the last HALF/FULL that another
  // ghost-clearing HALF is worth its cost. Measured on an X3: HALF 2186 ms against FAST 435 ms.
  //
  // Non-reader screens arm a HALF whenever they repaint everything after something else was on
  // the panel -- SettingsActivity does it on entry AND on returning from every child activity --
  // so bouncing in and out of a submenu paid ~2.2 s per return even when the HALF three updates
  // earlier had already cleared the panel.
  //
  // This gates ONLY that hygiene HALF. The correctness ones -- a stale differential baseline
  // after a framebuffer release, a reader exit -- are armed through setNextDisplayRefreshMode(),
  // and consumeRefreshOverride() replaces the requested mode outright, so they are unreachable
  // from here and cannot be suppressed.
  bool ghostClearingHalfWorthwhile() const { return fastRefreshesSinceFull_ >= GHOST_CLEARING_FAST_BUDGET; }
  // Make the write framebuffer match the currently displayed frame. Call before
  // a partial repaint that patches a few regions and re-displays without
  // re-rendering the full frame: displayBuffer() ends with swapBuffers(), so
  // the write buffer otherwise holds the frame from two refreshes ago. No-op
  // while the secondary is released or lent -- and a borrow does not fix the
  // write buffer up: it is still two refreshes old until the next full render.
  // Call this BEFORE borrowSecondaryBuffer() when the next draw may be an overlay.
  void syncWriteBufferFromDisplayed() const { display.syncWriteBufferFromActive(); }

  // Temporarily free the secondary (previous-frame) buffer (~52 KB) during
  // operations that don't need it (e.g. chapter compilation). BW rendering
  // continues normally. Grayscale AA and (on X4) fast differential are
  // unavailable until reallocSecondaryBuffer() is called.
  bool releaseSecondaryBuffer() const { return display.releaseSecondaryBuffer(); }
  bool reallocSecondaryBuffer() const { return display.reallocSecondaryBuffer(); }
  bool hasSecondaryBuffer() const { return display.hasSecondaryBuffer(); }
  // Borrow/return variant of release/realloc: the block never enters the heap,
  // so nothing can allocate inside it and the return cannot fail. See
  // FreeInkDisplay::borrowSecondaryBuffer.
  uint8_t* borrowSecondaryBuffer(size_t* size) const { return display.borrowSecondaryBuffer(size); }
  bool returnSecondaryBuffer() const { return display.returnSecondaryBuffer(); }
  // Keep fast differential alive (X4) after releaseSecondaryBuffer() by diffing
  // against the controller's retained baseline. See HalDisplay::setSingleBufferFastDiff.
  void setSingleBufferFastDiff(bool enabled) const { display.setSingleBufferFastDiff(enabled); }
  // What the panel does -- see HalDisplay::fastRefreshNeedsHostFrame and friends.
  bool fastRefreshNeedsHostFrame() const { return display.fastRefreshNeedsHostFrame(); }
  bool asyncRefreshHoldsFrameBuffer() const { return display.asyncRefreshHoldsFrameBuffer(); }
  bool ghostsUnderFastRefresh() const { return display.ghostsUnderFastRefresh(); }

  // True when triggerDisplayAsync() genuinely overlaps the waveform on this
  // panel. Ask this before spending the async gap on work; see
  // HalDisplay::supportsAsyncRefresh.
  bool supportsAsyncRefresh() const { return display.supportsAsyncRefresh(); }
  // What a FAST refresh costs on this panel, measured. 0 until one has run. See HalDisplay.
  uint16_t getLastFastRefreshMs() const { return display.getLastFastRefreshMs(); }

  // Non-blocking display split.
  // triggerDisplay() sends pixels, issues the refresh command and returns
  // immediately — the waveform runs in hardware. frameBuffer is safe to
  // overwrite after this returns. completeDisplay() genuinely sleeps (via
  // FreeRTOS semaphore) until BUSY deasserts, then does post-waveform work.
  // Both must be called from the render task; no other task may call SPI
  // display methods between triggerDisplay() and completeDisplay().
  // Honors a pending setNextDisplayRefreshMode() override (see consumeRefreshOverride);
  // defined out-of-line in the .cpp so it can share that logic with displayBuffer().
  void triggerDisplay(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;
  // X4 async refresh split: triggerDisplayAsync() returns while the waveform
  // runs; finishDisplayAsync() sleeps until it completes. CPU/RAM-only work is
  // allowed between the two calls (no display/SPI), same task as the trigger.
  // Honors the same refresh override + fading-fix policy as triggerDisplay().
  // On X3 the trigger falls back to the synchronous path and finish is a no-op.
  void triggerDisplayAsync(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;
  void finishDisplayAsync() const { display.finishDisplayAsync(); }
  void completeDisplay() const {
    display.completeDisplay();
    // No per-page RED reseed here — same rationale as displayBuffer(): the
    // display driver keeps the RED (previous-frame) plane current on every
    // refresh (writes RED from prev on dual-buffer fast, resyncs it after
    // single-buffer refreshes), so a syncRedRamFromFrameBuffer() here is a
    // redundant ~48 KB SPI write per page turn. The explicit seed lives at the
    // dual->single transition (release sites) only.
  }
  bool isRefreshPending() const { return display.isRefreshPending(); }
  bool isRedRamSynced() const { return display.isRedRamSynced(); }
  // Diagnostics: effective refresh mode of the last refresh (after any downgrade).
  HalDisplay::RefreshMode getLastRefreshMode() const { return display.getLastRefreshMode(); }
  // Diagnostics: last X4 displayMode byte (0x0C fast / 0x1C OTP-flash / 0xD4 half / 0x34 full).
  uint8_t getLastDisplayModeByte() const { return display.getLastDisplayModeByte(); }
  void displayWindow(int x, int y, int width, int height, bool turnOffScreen = false) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  // True from clearScreen() until the displayBuffer() that hands the frame to the panel: the part
  // of a render that draws (CPU, SD reads for bitmaps), not the refresh wait after it. Background
  // work on another task that shares the core and the card with rendering (Home's cover pass)
  // steps aside while this holds. A marker older than `maxAgeMs` counts as abandoned -- a
  // clearScreen() whose frame was never displayed -- so a caller can never wait on it forever.
  bool isComposingFrame(uint32_t maxAgeMs = 2000) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Extra bezel inset the reader has asked for, on top of whatever the board profile declares.
  //
  // The profile's viewableInsets describe where the CASE sits over the glass, measured per
  // board. They cannot describe how close is too close for a given pair of eyes, and on the
  // T5 S3 the plastic cover comes near enough to the live pixels that text at the profile inset
  // is genuinely hard to read. So the board says what is hidden and the reader says how much
  // clearance they want beyond it.
  //
  // Extra pixels, applied uniformly to all four edges. Pushed from main.cpp's loop alongside
  // setTextDarkness(), so a change in Settings takes effect on the next render without anything
  // having to remember to forward it; zero until something does, so a board nobody has
  // configured behaves exactly as before.
  void setViewablePadding(const int px) { viewablePadding.store(px, std::memory_order_relaxed); }

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;

  // Save / restore a rectangle of the framebuffer, in logical (orientation-
  // aware) coordinates. Pixels are packed 1bpp, row-major, MSB first, with a
  // set bit meaning "dark" -- so a region needs (width*height + 7) / 8 bytes.
  //
  // For an overlay that repaints a small moving element over an otherwise
  // unchanged frame: save what is under it, and restore that instead of
  // re-rendering the whole page to erase it. The dictionary's word-selection
  // highlight is the first caller -- without this, moving the cursor one word
  // costs a full two-pass page render, which also reloads every SD-font glyph
  // on the page.
  //
  // readFramebufferRegion returns the number of bytes written, or 0 when the
  // rectangle is empty, off-panel, or larger than bufferSize. Restoring a
  // region saved at a different size or position is undefined -- pass back the
  // same rectangle.
  size_t readFramebufferRegion(int x, int y, int width, int height, uint8_t* buffer, size_t bufferSize) const;
  void writeFramebufferRegion(int x, int y, int width, int height, const uint8_t* buffer) const;

  // Whether the pixel at logical (x, y) is dark. Bounds-checked: off-panel
  // reads report false rather than sampling a neighbouring row.
  bool getPixel(int x, int y) const;
  // Copy one packed 1bpp row in the device's physical portrait coordinate
  // space into the controller framebuffer. Source and framebuffer are MSB-first
  // with 0 = black, 1 = white; set invertBits when the source row uses 1 = ink.
  void writePhysicalPortraitPackedRow(int physicalY, const uint8_t* packedRow, int pixelWidth,
                                      bool invertBits = false) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int width, int height) const;
  // A borrowed 8-bit grayscale canvas (see borrowGray8Canvas): where drawing
  // routines accept one, they paint bytes into it instead of bits into the
  // framebuffer, so the panel quantises rather than the host.
  struct Gray8Target {
    uint8_t* canvas;
    uint16_t stride;
  };

  // When `gray8` is set the bitmap's 8-bit samples are painted there and the
  // framebuffer is left untouched — the 2-bit dither is skipped entirely rather
  // than done and discarded. Ignored for 1-bit bitmaps, which have no levels to
  // preserve. Default nullptr keeps every existing caller on the 2-bit path.
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  const Gray8Target* gray8 = nullptr) const;
  // One 8-bit sample at LOGICAL (x, y), rotated onto the canvas exactly as
  // drawPixel() rotates onto the framebuffer. Silently drops out-of-panel
  // coordinates, where drawPixel() logs — a scaled image legitimately walks off
  // the edge, and one log line per pixel would be its own failure.
  void drawGray8Pixel(const Gray8Target& gray8, int x, int y, uint8_t gray) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  // Unscaled primitives. The public accessors above apply the font's base scale; these do not,
  // and exist so the *Scaled paths can compound base x residual without applying base twice.
  int rawTextWidth(int fontId, const char* text, EpdFontFamily::Style style) const;
  int scaledTextAdvanceX(const EpdFontFamily& font, const char* text, EpdFontFamily::Style style, float scale) const;
  int rawLineHeight(int fontId) const;
  int rawFontAscenderSize(int fontId) const;
  void drawTextAtScale(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style,
                       float totalScale) const;
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, float scale) const;
  // Ink extents of `text` relative to its baseline, from glyph bitmap metrics:
  // aboveBaseline = tallest glyph ink top, belowBaseline = deepest ink below the
  // baseline (0 for caps/digits). Excludes the font's internal leading, unlike
  // getFontAscenderSize. Returns false when no glyph is found. SD fonts need
  // ensureFontReady(fontId, text) first so glyph metrics are loaded.
  bool getTextInkMetrics(int fontId, const char* text, EpdFontFamily::Style style, int* aboveBaseline,
                         int* belowBaseline) const;
  int getLineHeightScaled(int fontId, float scale) const;
  int getFontAscenderSizeScaled(int fontId, float scale) const;
  void drawTextScaled(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style,
                      float scale) const;
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

  // Grayscale functions
  void setRenderMode(const RenderMode mode) {
    // Absolute planes are a property of one grayscale pass, not of the renderer.
    // Returning to BW ends that pass, so the flag cannot outlive it and leak
    // into an unrelated draw.
    if (mode == BW) absoluteGrayPlanes.store(false, std::memory_order_relaxed);
    this->renderMode.store(static_cast<int>(mode), std::memory_order_relaxed);
  }
  RenderMode getRenderMode() const { return static_cast<RenderMode>(renderMode.load(std::memory_order_relaxed)); }

  // Switch the GRAYSCALE_LSB / GRAYSCALE_MSB passes from differential masks to
  // ABSOLUTE planes, for panels that report supportsAbsoluteGrayPlanes().
  // The encoding below is Bryan O'Sullivan's, from crosspoint-reader PR #3469;
  // see HalDisplay::supportsAbsoluteGrayPlanes() for what is his and what differs.
  //
  // Differential (the default): a plane bit is set only for a grey pixel, and
  // the B/W base supplies black and white — (LSB,MSB) is 00 black/white,
  // 11 dark, 01 light. Clear bits mean "leave this pixel as the base drew it".
  //
  // Absolute: the planes carry the whole image, so every pixel must be written
  // — (LSB,MSB) is 00 black, 10 dark, 01 light, 11 white. A clear bit is no
  // longer "leave as is", so anything the image does not cover has to be drawn
  // white rather than left at zero, and 1-bit overlays (button hints) must be
  // drawn into the planes too. Because white is 11 in both planes and black is
  // 00, ordinary BW drawing produces exactly the right bits for such an
  // overlay: set the mode back to BW and draw it normally.
  //
  // This affects drawBitmap() only. Glyph anti-aliasing and the reader's page
  // passes keep the differential encoding and the short nudge that suits them.
  void setAbsoluteGrayPlanes(const bool on) { absoluteGrayPlanes.store(on, std::memory_order_relaxed); }
  bool getAbsoluteGrayPlanes() const { return absoluteGrayPlanes.load(std::memory_order_relaxed); }

  // Passthroughs for the absolute-plane pass; see HalDisplay for the contract.
  bool supportsAbsoluteGrayPlanes() const;
  bool beginAbsoluteGrayPass(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;

  // Text darkness control:
  //   0 = Normal, 1 = Dark, 2 = Extra Dark, 3 = Maximum.
  // Only affects anti-aliased pixels in 2-bit (grayscale) glyph rendering;
  // 1-bit fonts and the BW pass are unchanged. See drawMaskFor2BitMode() in
  // GfxRenderer.cpp for the per-level pixel breakdown and a worked example.
  void setTextDarkness(const uint8_t d) { textDarkness.store(d, std::memory_order_relaxed); }
  uint8_t getTextDarkness() const { return static_cast<uint8_t>(textDarkness.load(std::memory_order_relaxed)); }

  // Armed by beginGrayCapture(); mutable because the whole render path is const.
  mutable uint8_t* grayCapLsb_ = nullptr;
  mutable uint8_t* grayCapMsb_ = nullptr;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  // Same, from a caller-owned panel-native plane rather than the framebuffer —
  // what the inline capture fills. The framebuffer overloads above are the
  // staged path, where each plane IS the framebuffer in turn.
  void copyGrayscaleLsbBuffers(const uint8_t* plane) const;
  void copyGrayscaleMsbBuffers(const uint8_t* plane) const;
  void displayGrayBuffer() const;

  // --- native grayscale (panels resolving more than the plane pipeline's 4 levels) ---
  //
  // Everything above encodes grey as two selector bits per pixel, because that is
  // all a KW controller can act on. Where the panel keeps a deeper buffer of its
  // own, a caller can skip the encoding: borrow that buffer, paint 8-bit grey into
  // it, and let the panel quantise at its native depth. See HalDisplay.

  // 4 on every dual-plane panel, more where the borrow below is backed.
  uint8_t getGrayLevels() const;
  // Physical panel layout, one byte per pixel, 0x00 black .. 0xFF white. nullptr
  // when the panel has no such buffer. Allocates nothing.
  uint8_t* borrowGray8Canvas(uint16_t* stride) const;
  // Stamp the framebuffer's BLACK pixels onto a borrowed canvas, leaving every
  // other canvas byte as painted. This is how 1-bit content — text drawn by the
  // normal render path — is composited over a grey image without being dithered
  // down with it. Both buffers are in physical layout, so no orientation applies.
  void stampBwOntoGray8Canvas(uint8_t* canvas, uint16_t stride) const;
  // Copy a LOGICAL rectangle of the framebuffer onto a borrowed canvas as solid
  // black and white, replacing whatever was painted there.
  //
  // The difference from stampBwOntoGray8Canvas() is opacity, and it decides which
  // one a caller wants. Stamping paints only the black pixels, so an image shows
  // through everywhere else — right for bare text. Content with a BACKGROUND, such
  // as a filled overlay panel or inverted white-on-black text, needs its whole
  // rectangle to land or the fill is dropped and the light pixels of the text with
  // it. Both buffers are 1-bit-derived, so nothing here is dithered.
  void compositeBwRectOntoGray8Canvas(const Gray8Target& gray8, int x, int y, int width, int height) const;
  void displayGray8Canvas() const;

  // Timing breakdown returned by renderGrayscalePlanesSequential().
  struct GrayscaleTimings {
    unsigned long planesMs = 0;   // LSB render+copy + MSB render+copy
    unsigned long displayMs = 0;  // displayGrayBuffer() waveform
    unsigned long restoreMs = 0;  // cleanupGrayscaleWithPreviousBuffer() SPI write
    bool aborted = false;         // dropped at the mid-pass abort point; no gray flush happened
  };

  // Both plane passes below take a shouldAbort predicate, evaluated once at the last point
  // where the pass can still be dropped without the user seeing anything: after the LSB plane
  // has been rendered into the framebuffer but before the first grayscale byte reaches the
  // controller. Aborting there costs only the framebuffer reseed that the normal tail performs
  // anyway, and gives back the MSB render, both plane writes and the gray flush — which is the
  // whole net cost of anti-aliasing. Pass a predicate that answers "is this page already on its
  // way out"; return false to always complete.

  // Abandon a grayscale pass without flushing it, and report it as aborted. Reseeds the
  // framebuffer and controller baseline from the previous-frame slot, exactly as a completed
  // pass's tail does. Valid at either gate: plane data may already sit in controller RAM, but
  // only displayGrayBuffer() runs the gray waveform, so nothing has reached the panel and the
  // BW page stays displayed.
  GrayscaleTimings abandonGrayscalePass(const unsigned long planesMs) {
    GrayscaleTimings t;
    t.planesMs = planesMs;
    const unsigned long tAbort = millis();
    setRenderMode(BW);
    cleanupGrayscaleWithPreviousBuffer();
    t.restoreMs = millis() - tAbort;
    t.aborted = true;
    return t;
  }

  // True when the panel can show the B/W base and its grayscale planes as ONE
  // waveform, i.e. when beginGrayCapture() + displayGrayscaleFrame() is
  // available instead of the base-then-overlay pair.
  bool supportsGrayFrame() const;
  // Display the framebuffer composed with the planes handed to
  // copyGrayscale*Buffers(), in one refresh. Resyncs the cached framebuffer
  // pointer afterwards for the same reason triggerDisplay() does: the display
  // swaps buffers, and every later draw must target the new write buffer.
  void displayGrayscaleFrame(HalDisplay::RefreshMode mode) const;
  // Deferred form: see HalDisplay::triggerGrayscaleFrame. Caller owes completeDisplay().
  void triggerGrayscaleFrame(HalDisplay::RefreshMode mode) const;

  // Render both grayscale planes sequentially into the BW framebuffer, streaming
  // each plane to the controller immediately after rendering it. No extra allocation
  // needed — the BW framebuffer is the scratch pad for both passes.
  //
  // After displayGrayBuffer(), cleanupGrayscaleWithPreviousBuffer() hands the driver
  // frameBufferActive -- the full BW page (including images) that displayBuffer() left
  // there before the grayscale pass began -- as the baseline for the next fast refresh,
  // and copies it back into the write buffer. With the secondary away (lent or released)
  // the driver is told there is no baseline and takes a clean sync on its next push; the
  // write buffer then keeps the last plane.
  //
  // renderFn is called twice (LSB, MSB). The RenderMode argument tells it which
  // pass is running. The caller sets setFastGrayscaleLut() before calling.
  //
  // Returns wall-clock timings for each of the three phases.
  //
  // Signature: void renderFn(RenderMode mode)
  template <typename RenderFn, typename AbortFn>
  GrayscaleTimings renderGrayscalePlanesSequential(RenderFn renderFn, AbortFn shouldAbort) {
    GrayscaleTimings t;
    const unsigned long t0 = millis();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_LSB);
    renderFn(GRAYSCALE_LSB);
    if (shouldAbort()) {
      return abandonGrayscalePass(millis() - t0);
    }
    copyGrayscaleLsbBuffers();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_MSB);
    renderFn(GRAYSCALE_MSB);
    // The MSB render is as long as the LSB one and can bail just as late, so it needs its own
    // gate: without it a plane abandoned part-way was still copied and flushed, putting a
    // half-drawn overlay on screen (seen on X3 as a COMPLETED pass with planes=545ms against a
    // normal ~890ms). Checked before the copy so no partial plane even reaches the controller.
    if (shouldAbort()) {
      return abandonGrayscalePass(millis() - t0);
    }
    copyGrayscaleMsbBuffers();

    const unsigned long t1 = millis();
    t.planesMs = t1 - t0;

    setRenderMode(BW);
    display.displayGrayBuffer(/*turnOffScreen=*/false);

    const unsigned long t2 = millis();
    t.displayMs = t2 - t1;

    // Restore the driver's baseline and the write buffer from frameBufferActive, which
    // holds the full BW page exactly as displayBuffer() left it. Using this
    // instead of re-rendering gives the correct baseline (images + text) and
    // costs only one SPI write.
    cleanupGrayscaleWithPreviousBuffer();

    t.restoreMs = millis() - t2;
    return t;
  }

  // Same plane dance as renderGrayscalePlanesSequential(), but entered while an
  // async BW refresh is still in flight (triggerDisplayAsync()): the LSB plane
  // renders into the write framebuffer DURING the waveform — CPU/RAM work only,
  // the controller scans its own RAM — then finishDisplayAsync() consumes the
  // remaining wait before the first SPI plane write. This lands the gray flush
  // ~one plane-render earlier than deferring the whole pass to after the
  // waveform, which is what makes the AA touch-up read as part of the page
  // refresh instead of a separate later update.
  //
  // Caller contract: an async refresh MUST be in flight, and the glyphs the
  // renderFn draws must already be prewarmed (a cache miss would issue SD reads
  // — allowed — but a display/SPI call in renderFn is not).
  template <typename RenderFn, typename AbortFn>
  GrayscaleTimings renderGrayscalePlanesInterleaved(RenderFn renderFn, AbortFn shouldAbort) {
    GrayscaleTimings t;
    const unsigned long t0 = millis();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_LSB);
    renderFn(GRAYSCALE_LSB);
    const unsigned long tLsbDone = millis();

    // Waveform still running: sleep out the remainder (power hooks active).
    display.finishDisplayAsync();
    const unsigned long tWaveDone = millis();

    // Abort point. This is the ideal place for it on the interleaved path: the BW page is now
    // fully on screen, and the LSB render just spent was paid for out of the waveform wait
    // rather than out of the page-turn budget — so dropping the pass here reclaims the entire
    // net cost of the AA and forfeits nothing that was actually charged to the user.
    if (shouldAbort()) {
      return abandonGrayscalePass(tLsbDone - t0);
    }

    copyGrayscaleLsbBuffers();
    clearScreen(0x00);
    setRenderMode(GRAYSCALE_MSB);
    renderFn(GRAYSCALE_MSB);
    // Second gate, same reason as in the sequential pass: an MSB render that bailed part-way
    // must never be copied or flushed. Checked before the copy.
    if (shouldAbort()) {
      return abandonGrayscalePass((tLsbDone - t0) + (millis() - tWaveDone));
    }
    copyGrayscaleMsbBuffers();

    const unsigned long t1 = millis();
    // Report only actual plane work; the residual waveform sleep is not ours.
    t.planesMs = (tLsbDone - t0) + (t1 - tWaveDone);

    setRenderMode(BW);
    display.displayGrayBuffer(/*turnOffScreen=*/false);

    const unsigned long t2 = millis();
    t.displayMs = t2 - t1;

    cleanupGrayscaleWithPreviousBuffer();

    t.restoreMs = millis() - t2;
    return t;
  }

  // X3-only: trade AA visual fidelity for ~2.2 s faster page-flip wall clock.
  // No effect on X4 (its single grayscale LUT already runs at ~500 ms).
  void setFastGrayscaleLut(bool fast) const { display.setFastGrayscaleLut(fast); }
  bool getFastGrayscaleLut() const { return display.getFastGrayscaleLut(); }

  // Active pixel-write target for raw writers that bypass drawPixel for speed.
  // Returns the full framebuffer and its extent ([0, panelHeight)).
  // --- Inline grayscale capture -----------------------------------------------
  // While armed, the 2-bit glyph path ALSO writes the two anti-aliasing planes
  // as it draws the B/W page, so one walk over the page produces base and greys
  // together. The alternative — and what this replaces — is rendering the whole
  // page twice more, once per plane, purely to re-derive pixels the B/W pass
  // already had in its hand.
  //
  // Idea adopted from jetaudio's crosspoint-aurora (GfxRenderer::beginGrayCapture
  // / captureGray, used by its single-push reader path). Aurora hooks a per-pixel
  // callback; this fork's 2-bit path is a fused gather+threshold that builds a
  // row/column mask per 8-pixel chunk, so the same effect is had by running that
  // blit once more per plane with the plane's own draw mask — the page walk,
  // layout and glyph decode still happen exactly once, which is where the cost is.
  //
  // Planes are PANEL-NATIVE (panelWidthBytes * panelHeight), the same geometry
  // copyGrayscale*Buffers expects, and the caller clears them. Only meaningful
  // during a BW pass; the grayscale passes ignore it.
  void beginGrayCapture(uint8_t* lsbPlane, uint8_t* msbPlane) const {
    grayCapLsb_ = lsbPlane;
    grayCapMsb_ = msbPlane;
  }
  void endGrayCapture() const { grayCapLsb_ = grayCapMsb_ = nullptr; }
  [[nodiscard]] bool grayCaptureActive() const { return grayCapLsb_ != nullptr && grayCapMsb_ != nullptr; }
  [[nodiscard]] uint8_t* grayCaptureLsb() const { return grayCapLsb_; }
  [[nodiscard]] uint8_t* grayCaptureMsb() const { return grayCapMsb_; }

  uint8_t* getWriteTarget() const { return frameBuffer; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return cacheOnlyImageWrites_ ? 0 : static_cast<int>(panelHeight); }
  // While one of these is live the raw pixel writers see a zero-row write window and touch no
  // framebuffer byte, while the decoders' pixel caches are written as usual: an image can be
  // decoded into its .pxc without the displayed frame changing. The reader's image lane uses it
  // to warm the caches of the next few pages between page turns (memory audit 2026-09, R7).
  struct ScopedCacheOnlyImageWrites {
    explicit ScopedCacheOnlyImageWrites(const GfxRenderer& r) : renderer_(r) { renderer_.cacheOnlyImageWrites_ = true; }
    ~ScopedCacheOnlyImageWrites() { renderer_.cacheOnlyImageWrites_ = false; }
    ScopedCacheOnlyImageWrites(const ScopedCacheOnlyImageWrites&) = delete;
    ScopedCacheOnlyImageWrites& operator=(const ScopedCacheOnlyImageWrites&) = delete;

   private:
    const GfxRenderer& renderer_;
  };
  bool isStripActive() const { return false; }
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }

  bool storeBwBuffer();                                         // Returns true if buffer was stored successfully
  bool storeBwBufferRect(int x, int y, int width, int height);  // Store only rows intersecting logical rect
  void restoreBwBuffer();                                       // Restore and free the stored buffer
  // Re-syncs the controller's RED RAM from the current BW framebuffer so the
  // next differential page turn has a clean baseline. Called after the tiled
  // grayscale path, which leaves the panel's gray planes loaded but the BW
  // framebuffer untouched.
  //
  // The FreeInk drivers stream planes row-reversed (EpdBus::sendPlaneFlipped) and never
  // modify the framebuffer, so these calls leave it untouched.
  //
  // Safe to call on any panel: one whose FAST refresh needs no host frame
  // (fastRefreshNeedsHostFrame() false) has no RED to seed, and the call returns at once.
  void syncRedRamFromFrameBuffer() const;
  void cleanupGrayscaleWithFrameBuffer() const;
  // Restore the driver's baseline and the write buffer from the display's previous-frame
  // buffer (FreeInkDisplay's frameBufferActive), which holds the full BW page that
  // displayBuffer() committed before the grayscale pass, images included. With the
  // secondary away the driver is told there is no baseline and takes a clean sync on its
  // next push.
  void cleanupGrayscaleWithPreviousBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyphRef& glyph) const;

  // Scaled-glyph mask cache (see ScaledGlyphEntry). Public only because the glyph
  // pipeline lives in free functions in GfxRenderer.cpp; treat as internal.
  // find returns the cached mask for an exact key match, alloc reserves a zeroed
  // mask of w*h bits for the caller to fill, or nullptr when the glyph is too
  // large to cache or the arena could not be allocated.
  const uint8_t* findScaledGlyphMask(const void* fontData, uint32_t cp, float scale, uint8_t sel, int w, int h) const;
  uint8_t* allocScaledGlyphMask(const void* fontData, uint32_t cp, float scale, uint8_t sel, int w, int h) const;

  // Drop every cached mask, keeping the arena. MUST run whenever a font is added
  // or removed: entries are keyed by EpdFontData address, which a later font
  // object could reuse.
  void invalidateScaledGlyphCache() const {
    scaledGlyphCount_ = 0;
    scaledGlyphUsed_ = 0;
  }
  // Everything glyph drawing keeps between pages: FontCacheManager's page slots and the
  // scaled-glyph cache. Readers give it back on exit, so Home, Settings and the font selector do
  // not run 7-12 KB short after every book (X3, 2026-10-07: 5,064 B + ~2 KB, or 10,056 B + ~2 KB
  // after a synthesised body size); the next reader takes it again in onEnter. Network sessions
  // give it back too (trimMemoryForNetworkSession).
  void releaseGlyphCaches() const;

  // Give the ~4 KB arena back. The cache re-allocates lazily if rendering resumes.
  void releaseScaledGlyphCache() const {
    scaledGlyphEntries_.reset();
    scaledGlyphArena_.reset();
    scaledGlyphOom_ = false;
    invalidateScaledGlyphCache();
  }

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;

  // Release both display frame buffers back to the heap (~96-104KB total).
  // Nulls the local frameBuffer pointer too; displayBuffer() rejects flushes
  // (LOG_ERR + no-op) while it is null instead of streaming freed memory.
  // Only valid after the final displayBuffer(); the device must reboot before
  // any display operation is attempted again.
  void releaseFrameBuffers() {
    display.releaseBuffers();
    frameBuffer = nullptr;
    releaseScaledGlyphCache();
  }

  // Release both display buffers and install a caller-owned scratch buffer as
  // the active framebuffer. Pixel writes during the warm pass land in scratch
  // (discarded on reboot) while the decoder can use the freed ~96 KB for its
  // own allocation. scratchSize must be >= panelWidthBytes * panelHeight.
  // The device must reboot before any display operation is attempted again.
  bool releaseFrameBuffersWithScratch(uint8_t* scratch, size_t scratchSize) {
    if (!scratch || scratchSize < static_cast<size_t>(panelWidthBytes) * panelHeight) return false;
    display.releaseBuffers();
    releaseScaledGlyphCache();
    memset(scratch, 0, scratchSize);
    frameBuffer = scratch;
    return true;
  }
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache helpers: operate on a logical (orientation-aware) rect and
  // copy only the framebuffer bytes it touches. Used by HomeActivity to snapshot
  // the cover tile (~16 KB) instead of the full 48 KB framebuffer.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
