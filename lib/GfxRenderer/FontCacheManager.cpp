#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <ShapingTokens.h>
#include <TtfEpdFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts,
                                   const std::map<int, TtfEpdFont*>& ttfFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts), ttfFonts_(ttfFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
#if CROSSPOINT_VECTOR_FONTS
  for (auto& [id, font] : ttfFonts_) {
    if (font) font->clearCache();
  }
#endif
}

void FontCacheManager::releaseSdFontCaches() {
  lastScanFontCount_ = 0;
  memset(scanStyleMasks_, 0, sizeof(scanStyleMasks_));
  memset(lastScanStyleMasks_, 0, sizeof(lastScanStyleMasks_));
  if (releaseCb_) releaseCb_(releaseCtx_);
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseResidentCaches();
  }
#if CROSSPOINT_VECTOR_FONTS
  for (auto& [id, font] : ttfFonts_) {
    if (font) font->releaseResidentCaches();
  }
#endif
}

bool FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask, bool accumulate) {
  // TTF (vector) font prewarm path. This is the single dispatch every draw path
  // funnels through (reader endScanAndPrewarm, the settings preview, UI text),
  // so building here covers them all. For TTF, build() maintains an accumulating
  // cache across render scopes, flushing only on capacity exhaustion or degradation;
  // addCoverage() faults incremental codepoints without evicting nearer pages.
#if CROSSPOINT_VECTOR_FONTS
  auto tit = ttfFonts_.find(fontId);
  if (tit != ttfFonts_.end() && tit->second) {
    bool ok = true;
    for (uint8_t i = 0; i < 4; ++i) {
      if (styleMask & (1 << i)) {
        const bool stepOk = accumulate ? tit->second->addCoverage(utf8Text, i) : tit->second->build(utf8Text, i);
        ok = ok && stepOk;
      }
    }
    return ok;
  }
#endif

  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask, /*metadataOnly=*/false, /*loadKernLig=*/true, accumulate);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return true;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return true;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
  return true;
}

void FontCacheManager::compactAfterPrewarm(const bool dropLayoutAdvanceCache) {
  for (auto& [id, font] : sdCardFonts_) {
    if (!font) continue;
    uint8_t activeMask = 0;
    for (uint8_t slot = 0; slot < lastScanFontCount_; ++slot) {
      if (lastScanFontIds_[slot] == id) {
        activeMask = lastScanStyleMasks_[slot];
        break;
      }
    }
    activeMask |= font->scannedStyleMask();
    font->compactForRender(activeMask, dropLayoutAdvanceCache);
  }
}

int FontCacheManager::getIdleLookaheadPages(const int fontId) const {
#if CROSSPOINT_VECTOR_FONTS
  return ttfFonts_.count(fontId) > 0 ? 5 : 1;
#else
  (void)fontId;
  return 1;
#endif
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

uint8_t FontCacheManager::resolveScanStyle(int fontId, EpdFontFamily::Style style) const {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end()) return sdFont->second->resolveStyle(baseStyle);

  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return baseStyle;

  const EpdFontData* resolvedData = font->second.getData(static_cast<EpdFontFamily::Style>(baseStyle));
  for (uint8_t candidate = 0; candidate < 4; candidate++) {
    if (font->second.getData(static_cast<EpdFontFamily::Style>(candidate)) == resolvedData) return candidate;
  }
  return baseStyle;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end() && sdFont->second) {
    sdFont->second->noteScannedStyle(baseStyle);
  }

  uint8_t fontSlot = scanFontCount_;
  for (uint8_t i = 0; i < scanFontCount_; i++) {
    if (scanFontIds_[i] == fontId) {
      fontSlot = i;
      break;
    }
  }
  if (fontSlot == scanFontCount_) {
    if (scanFontCount_ >= MAX_SCAN_FONTS) return;
    scanFontIds_[scanFontCount_++] = fontId;
  }

  const uint8_t resolvedStyle = resolveScanStyle(fontId, style);
  const uint8_t group = fontSlot * 4 + resolvedStyle;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;
    if (shaping::isPositionToken(codepoint)) continue;  // modifies the next glyph; has no glyph of its own
    scanStyleMasks_[fontSlot] |= static_cast<uint8_t>(1u << resolvedStyle);
    // Small caps draw lowercase letters with their uppercase glyphs.
    if (style & EpdFontFamily::SMALL_CAPS) codepoint = utf8SmallCapsUpper(codepoint);

    const uint32_t packed = (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) |
                            (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT) | codepoint;
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == packed) {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager, const bool accumulate)
    : manager_(&manager), accumulate_(accumulate) {
  manager_->scanMode_ = ScanMode::Scanning;
  if (!accumulate_) {
    manager_->clearCache();
    for (auto& [id, font] : manager_->sdCardFonts_) {
      (void)id;
      if (font) font->beginPrewarmScope();
    }
    manager_->resetStats();
  }
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  manager_->scanOverflowWarned_ = false;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
  memset(manager_->scanStyleMasks_, 0, sizeof(manager_->scanStyleMasks_));
}

bool FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  if (manager_->scanMode_ != ScanMode::Scanning) return true;
  manager_->scanMode_ = ScanMode::None;

  // Record per-font style masks for compactAfterPrewarm(), including styles
  // observed past MAX_SCAN_CODEPOINTS.
  if (!accumulate_) {
    manager_->lastScanFontCount_ = 0;
    memset(manager_->lastScanStyleMasks_, 0, sizeof(manager_->lastScanStyleMasks_));
  }
  for (uint8_t slot = 0; slot < manager_->scanFontCount_; ++slot) {
    const int id = manager_->scanFontIds_[slot];
    const uint8_t mask = manager_->scanStyleMasks_[slot];
    bool merged = false;
    for (uint8_t dst = 0; dst < manager_->lastScanFontCount_; ++dst) {
      if (manager_->lastScanFontIds_[dst] == id) {
        manager_->lastScanStyleMasks_[dst] |= mask;
        merged = true;
        break;
      }
    }
    if (!merged && manager_->lastScanFontCount_ < MAX_SCAN_FONTS) {
      const uint8_t dst = manager_->lastScanFontCount_++;
      manager_->lastScanFontIds_[dst] = id;
      manager_->lastScanStyleMasks_[dst] = mask;
    }
  }

  if (manager_->scanCodepointCount_ == 0) {
    manager_->scanFontCount_ = 0;
    memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
    memset(manager_->scanStyleMasks_, 0, sizeof(manager_->scanStyleMasks_));
    return true;
  }

  std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_);

  uint16_t groupStarts[SCAN_GROUP_COUNT] = {};
  uint16_t groupUtf8Lens[SCAN_GROUP_COUNT] = {};
  for (uint8_t group = 1; group < SCAN_GROUP_COUNT; group++) {
    groupStarts[group] = groupStarts[group - 1] + manager_->scanGroupCounts_[group - 1];
  }

  // Phase 1: encode each group's packed codepoints into UTF-8 bytes in place.
  // Each 4-byte entry holds one UTF-8 codepoint (1..4 bytes), so encoded bytes
  // stay within the group's own slice before termination.
  for (int group = SCAN_GROUP_COUNT - 1; group >= 0; group--) {
    const uint16_t groupCount = manager_->scanGroupCounts_[group];
    if (groupCount == 0) continue;

    const uint16_t groupStart = groupStarts[group];
    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStart);
    char* output = utf8Text;
    for (uint16_t i = 0; i < groupCount; i++) {
      const uint32_t codepoint = manager_->scanCodepoints_[groupStart + i] & SCAN_CODEPOINT_MASK;
      output = utf8AppendCodepoint(output, codepoint);
    }
    groupUtf8Lens[group] = static_cast<uint16_t>(output - utf8Text);
  }

  // Phase 2: prewarm in forward order so fontSlot=0, style=0 (Regular body text)
  // claims resident slabs before Bold/Italic/BoldItalic. Save and restore the
  // boundary byte in case a group of 4-byte codepoints fills its entire slice.
  bool ok = true;
  for (uint8_t group = 0; group < SCAN_GROUP_COUNT; group++) {
    if (manager_->scanGroupCounts_[group] == 0) continue;

    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStarts[group]);
    const uint16_t utf8Len = groupUtf8Lens[group];
    const char savedByte = utf8Text[utf8Len];
    utf8Text[utf8Len] = '\0';

    const uint8_t fontSlot = group / 4;
    const uint8_t style = group & 0x03;
    const bool stepOk = manager_->prewarmCache(manager_->scanFontIds_[fontSlot], utf8Text, 1 << style, accumulate_);
    ok = ok && stepOk;
    utf8Text[utf8Len] = savedByte;
  }

  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
  memset(manager_->scanStyleMasks_, 0, sizeof(manager_->scanStyleMasks_));
  return ok;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called
    if (!accumulate_) manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_), accumulate_(other.accumulate_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope(const bool accumulate) {
  return PrewarmScope(*this, accumulate);
}
