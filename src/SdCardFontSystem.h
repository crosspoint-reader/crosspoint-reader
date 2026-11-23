#pragma once

#include <Epub/blocks/TextBlock.h>
#include <HalStorage.h>  // HalFile (kept open for streamed TTFs)
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <VectorFontSupport.h>

#if CROSSPOINT_VECTOR_FONTS
#include <FontPsram.h>  // PsramVector for resident TTF bytes
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class EpdFontFamily;
class TtfEpdFont;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  // Constructor and destructor are out-of-line (defined in the .cpp where
  // TtfEpdFont is a complete type) so the std::unique_ptr<TtfEpdFont> member
  // can be constructed/destroyed with only a forward declaration visible here.
  SdCardFontSystem();
  ~SdCardFontSystem();
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer, const EpdFontFamily& small, const EpdFontFamily& ui10, const EpdFontFamily& ui12);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// The caller holds RenderLock after startup.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  /// Settings previews can defer retries so the displayed family list stays stable.
  void ensureLoaded(GfxRenderer& renderer, bool retryUnavailable = true);

  static bool supportsUiFont(const SdCardFontFamilyInfo& family);

  /// Bind the UI font to an installed family or the built-ins (empty name).
  /// Returns false on failure, with the built-ins restored. The caller holds
  /// RenderLock after startup.
  bool applyUiFont(GfxRenderer& renderer, const char* name);
  const char* activeUiFontName() const;
#if CROSSPOINT_VECTOR_FONTS
  bool uiTtfFailed() const { return uiTtfFailed_.load(std::memory_order_relaxed); }
  void recoverUiFont(GfxRenderer& renderer);
#endif

  /// Release SD font handles and restore built-ins before storage leaves firmware control.
  /// The caller holds RenderLock.
  void releaseForStorage(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  const SdCardFontRegistry& registry() const { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty();

  /// Re-scan after an explicit font refresh, rearming transient retries.
  void refreshRegistry();

 private:
  bool loadUiCpfonts(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  void restoreBuiltinUiFonts(GfxRenderer& renderer);
  void clearCpfontUiFallbacks(GfxRenderer& renderer);
  void handleMissingFamily() const;
  void ensureReaderFontLoaded(GfxRenderer& renderer, bool registryWasDirty);

  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched script fallback for the corresponding UI font, so book
  // titles/list rows in scripts the built-ins lack (CJK, Greek, Cyrillic, ...)
  // render at the same size as the surrounding Latin UI text. No-op when no SD
  // family is loaded. Safe to call repeatedly (sizes already loaded are
  // reused).
  void setupUiFallbacks(GfxRenderer& renderer);

#if CROSSPOINT_VECTOR_FONTS
  // --- Vector (.ttf/.otf) font path (FreeInkFont via TtfEpdFont) -------------
  // Load/refresh the selected TTF family at the current reader size, register
  // it with the renderer, and track it so ensureSdCardFontReady() rebuilds its
  // glyph set per page. registryWasDirty forces a reload even if unchanged.
  void loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, bool registryWasDirty,
                     bool setupFallbacks = true);
  // Unregister + free the active TTF font (and its UI-size fallbacks), if any.
  void unloadTtf(GfxRenderer& renderer);
  // Register the loaded TTF at each built-in UI size as a script fallback, so UI
  // text (book titles, list rows, menus, status bar) in scripts the built-in
  // fonts lack renders in the chosen TTF. Mirrors setupUiFallbacks for .cpfont.
  void setupTtfUiFallbacks(GfxRenderer& renderer);
  void clearTtfUiFallbacks(GfxRenderer& renderer);
  // Style source arrays hold 4 entries: 0=regular (required), 1=bold,
  // 2=italic, 3=bold-italic. The reader and UI can share the same slot.
  struct TtfSource;
  // Open one style source file (resident if small, streamed if large).
  // Returns false on open/read failure.
  static bool openTtfSource(TtfSource& s, const std::string& path);
  // Open every style file the family ships. False if regular failed to open.
  bool openTtfFamilySources(const SdCardFontFamilyInfo& family, TtfSource* sources);
  // Register every present source with `font` (shared bytes / file handles).
  static void addTtfSources(TtfEpdFont& font, TtfSource* sources);
  // Close/free all style sources.
  void releaseTtfSources(TtfSource*& sources, const TtfSource* other);
  TtfSource* unusedTtfSources(const TtfSource* other);
  bool canShareTtfSources(const TtfSource* sources) const;
  // Load `family` at the three UI sizes and bind the UI font slots to it.
  bool loadUiTtf(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  // Unbind and free the vector UI font, if one is loaded.
  void unloadUiTtf(GfxRenderer& renderer);
  // ReadFn for streamed sources: serves the PSRAM prefix cache first, SD after.
  static unsigned long prefixRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
#endif  // CROSSPOINT_VECTOR_FONTS

  // --- Sized variants of the reader font (CSS font-size, headings, drop caps) ---
  // Served to the renderer through FontVariantProvider. Variant ids derive from the
  // reader font id and point size, so ids cached in section files can be reloaded after
  // eviction or a reboot. Least recently used variants are evicted at the caps below.
  int resolveVariant(int fontId, float scale);
  bool loadVariant(int fontId);
  bool loadCpfontVariant(const SdCardFontFamilyInfo& family, uint8_t pointSize, int fontId);
  void unloadCpfonts(GfxRenderer& renderer);
  static int variantFontId(int readerFontId, uint8_t pointSize);

  struct CpfontVariant {
    int fontId;
    uint32_t lastUse;
  };
  static constexpr size_t MAX_CPFONT_VARIANTS = TextBlock::MAX_WORD_FONTS + 2;  // Inline, block, and drop-cap fonts.
  std::vector<CpfontVariant> cpfontVariants_;
  uint32_t variantClock_ = 0;
  GfxRenderer* renderer_ = nullptr;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  SdCardFontManager uiCpfonts_;
  int cpfontUiIds_[3] = {};
  const EpdFontFamily* builtinUiFonts_[3] = {};
  char activeUiFontName_[SdCardFontRegistry::FAMILY_NAME_CAPACITY] = "";
  std::atomic<bool> registryDirty_{false};

#if CROSSPOINT_VECTOR_FONTS
  void scanSavedFamilies(bool automaticRetry);
  bool readerFontUnavailable_ = false;
  bool uiFontRecoveryPending_ = false;
  // A successful scan must not rearm the other role's spent retry.
  bool readerFontRetrySpent_ = false;
  bool uiFontRetrySpent_ = false;
  std::atomic<bool> uiTtfFailed_{false};

  // One style source file. SMALL files are read fully into `bytes` (resident,
  // PSRAM when present); LARGE files stream from `file` (kept open) so a multi-MB
  // file never sits in RAM. All faces (reader + UI sizes) share these sources.
  struct TtfSource {
    // Resident form: the whole file. Streamed form: a PSRAM prefix cache of the
    // file head (cmap/loca/hmtx) — empty when PSRAM couldn't fund it.
    freeink::font::PsramVector<uint8_t> bytes;
    HalFile file;  // open handle (streamed form)
    bool streamed = false;
    unsigned long size = 0;
    bool present = false;
    std::atomic<bool>* uiReadFailed = nullptr;
  };

  // Sources outlive the FreeType faces that borrow them. The reader and UI
  // can each use a slot or share one slot when both use the same family.
  TtfSource sourceSlots_[2][4];
  // Zero for an unused slot or one missing a selected style.
  uint32_t sourceSlotGenerations_[2] = {};
  std::unique_ptr<TtfEpdFont> ttf_;
  TtfSource* ttfSources_ = nullptr;
  std::string ttfFamily_;     // loaded vector family name ("" = none)
  int ttfFontId_ = 0;         // renderer font id for ttf_ (0 = none)
  uint8_t ttfPointSize_ = 0;  // size ttf_ was built at
  // UI-size TTF fallbacks (share ttfSources_); parallel to their renderer font ids.
  std::vector<std::unique_ptr<TtfEpdFont>> ttfUi_;
  std::vector<int> ttfUiIds_;

  // Reader-font faces at other sizes (share ttfSources_).
  struct TtfVariant {
    int fontId;
    uint32_t lastUse;
    std::unique_ptr<TtfEpdFont> font;
  };
  static constexpr size_t MAX_TTF_VARIANTS = TextBlock::MAX_WORD_FONTS + 2;
  std::vector<TtfVariant> ttfVariants_;
  bool loadTtfVariant(uint8_t pointSize, int fontId);
  void clearTtfVariants(GfxRenderer& renderer);

  // UI and reader can share a current, complete source slot for the same family.
  // uiTtf_[0] owns the FreeType faces; the other two are sized variants of it.
  TtfSource* uiTtfSources_ = nullptr;
  std::unique_ptr<TtfEpdFont> uiTtf_[3];
#endif  // CROSSPOINT_VECTOR_FONTS
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
