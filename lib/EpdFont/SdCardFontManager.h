#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class EpdFontFamily;
class GfxRenderer;
template <typename Family>
struct ResolvedFont;
class BuildArena;
class SdCardFont;
class NorFlash;
class TtfFont;
struct SdCardFontFamilyInfo;

// Whether a load may populate the flash font partition on a cache miss.
// Writing it erases the whole partition and copies the family back from SD, so
// only a load that the reader will actually use should pay that price.
enum class FlashCachePolicy : uint8_t {
  ReadWrite,  // mmap on a hit, write-then-mmap on a miss
  ReadOnly,   // mmap on a hit, plain SD read on a miss
};

class SdCardFontManager {
 public:
  SdCardFontManager();
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the single size closest to targetPtSize for a discovered family.
  // Only one .cpfont file is loaded; other sizes remain on disk. This keeps
  // resident interval + kern/ligature tables to one size's worth of memory
  // (see PR #1327 discussion re: Literata OOM).
  // Returns true on success.
  // onColdLoad (if set) is invoked once, just before the slow path that writes the
  // font into the flash partition (i.e. only on a genuine first load, not on a
  // flash-cache mmap hit) — callers use it to show a "loading font" popup.
  // Under FlashCachePolicy::ReadOnly that slow path is skipped entirely and
  // onColdLoad never fires.
  // `arena`, when set, is handed to the new SdCardFont (SdCardFont::useArena): the caller owns the
  // block and must unload this family before rewinding it. Only font previews pass one.
  //
  // A TTF family (SdCardFontFamilyInfo::ttf) is drawn by a TtfFont at targetPtSize itself, from the
  // glyph store in the same partition; onColdLoad fires before it is installed there. Read-only
  // (preview) loads of a TTF family are refused.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                  const std::function<void()>& onColdLoad = {}, FlashCachePolicy policy = FlashCachePolicy::ReadWrite,
                  BuildArena* arena = nullptr);

  // Serve targetPtSize from the face that is loaded. A size the family ships is that face; any
  // other size is an ALIAS: a second font ID bound to the same face with a base scale of
  // target / loaded (GfxRenderer::insertScaledFont), so the reader renders the chosen typeface at
  // the chosen size instead of a built-in family. No .cpfont in existence ships 20-26 pt, so
  // every existing card reaches the larger sizes this way.
  //
  // One alias at a time: the reader wants one body size, and the ID is derived from
  // (content hash, family, target size) so a paginated section keyed on it is valid across
  // sessions. Returns false when nothing is loaded or the derived ID collides with a registered
  // font, in which case that size resolves to 0 and the caller falls back.
  bool ensureSizeAlias(GfxRenderer& renderer, uint8_t targetPtSize);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // The loaded TTF family's body size: a new font ID drawn natively at pointSize, never scaled.
  // False when no TTF family is loaded, the size is out of range, or its ID collides.
  bool setTtfBodySize(GfxRenderer& renderer, uint8_t pointSize);
  // Runs the TTF glyph store's pending wipe (it filled up), calling onBusy first. Only at book open.
  void wipeTtfIfPending(const std::function<void()>& onBusy);
  bool ttfLoaded() const { return ttf_ != nullptr; }

  // The loaded TTF family's native sizes (headings, CSS sizes, drop caps at their own pixel size). The renderer
  // asks for a size's font ID the first time something is laid out or drawn with it (the resolver installed at
  // load), and the face is made then. Their IDs come from a window FontSizeLadder::setNative() computes too.
  struct NativeSizes {
    int32_t base = 0;       // font ID base | px
    uint32_t bodyPpem = 0;  // the body face's pixels per em, 26.6
    uint8_t minPx = 0;
    uint8_t maxPx = 0;
  };
  // For a body font ID of the loaded TTF family; nothing for any other font, or when no window was free.
  std::optional<NativeSizes> ttfNativeSizes(int bodyFontId) const;
  // Forgets every native size: their font IDs leave the renderer and their faces are freed. Only where nothing laid
  // out with them is in use: book open, a body size change, unload.
  void dropTtfNativeSizes(GfxRenderer& renderer);

  // Font ID that renders the loaded family AT pointSize: the face itself when that is the loaded
  // size, the alias when one was made for it, else 0 -- never a nearby size. The setting says
  // "24pt" and the page has to be 24pt; a quiet 18pt is indistinguishable from the feature not
  // working for exactly the reader it exists for.
  int getFontId(const std::string& familyName, uint8_t pointSize) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);
  void dropSizeAlias(GfxRenderer& renderer);
  bool loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                     const std::function<void()>& onColdLoad);
  void registerTtfFace(GfxRenderer& renderer, int fontId);
  int32_t pickNativeBase(const GfxRenderer& renderer, uint32_t identityHash, const char* familyName) const;
  bool fontIdTaken(const GfxRenderer& renderer, int fontId) const;
  static std::optional<ResolvedFont<EpdFontFamily>> resolveTtfSize(void* ctx, int fontId);

  GfxRenderer* renderer_ = nullptr;
  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
  // The scaled alias for the current target size (see ensureSizeAlias); 0 when the loaded face
  // serves the target size itself.
  int aliasFontId_ = 0;
  uint8_t aliasPointSize_ = 0;
  // The loaded TTF family, if any: its font area (EspNorFlash) and the font drawn from it. The
  // font is destroyed first, so the store is closed before the flash goes.
  std::unique_ptr<NorFlash> ttfFlash_;
  std::unique_ptr<TtfFont> ttf_;
  uint32_t ttfIdentityHash_ = 0;
  int ttfFontId_ = 0;
  int32_t ttfNativeBase_ = 0;  // the native sizes' ID window; 0: none
};
