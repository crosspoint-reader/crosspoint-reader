#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // Release every rebuildable SD-font cache (mini glyph/kern arenas, kern/lig
  // class tables, overflow rings, advance tables) while keeping the fonts
  // loaded. Everything faults back in on demand. For heap-critical transitions
  // (e.g. web-server + WiFi startup); see SdCardFont::releaseResidentCaches().
  void releaseSdFontCaches();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // Per-font scan accumulators. A page mixes font ids (reader font, UI font,
  // and the SD fallback the CJK-bearing strings resolve to); prewarming only
  // the first-recorded id would leave the other fonts to fault glyph-by-glyph
  // during the real draw pass. Fixed-size array: a render pass touches at most
  // a handful of font ids, and entries past the cap are simply not batched
  // (they fall back to the per-string prewarm in GfxRenderer).
  static constexpr uint8_t MAX_SCAN_FONTS = 4;
  struct ScanEntry {
    // Occupancy is tracked separately: font ids are FNV hashes cast to int
    // (SdCardFontManager::computeFontId, src/fontIds.h) and are routinely
    // negative, so no id value can serve as the "free slot" sentinel.
    bool used = false;
    int fontId = 0;
    std::string text;
    uint8_t styleMask = 0;
  };
  ScanEntry scanEntries_[MAX_SCAN_FONTS];
  void resetScanEntries();
};
