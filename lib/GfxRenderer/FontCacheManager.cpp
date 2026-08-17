#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseSdFontCaches() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseResidentCaches();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!epdFontUsesDecompressor(data)) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
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
  if (!text || !*text) return;
  if (scanFontId_ < 0) scanFontId_ = fontId;

  const uint8_t resolvedStyle = resolveScanStyle(fontId, style);
  uint32_t insertOffset = 0;
  for (uint8_t i = 0; i <= resolvedStyle; i++) {
    insertOffset += scanStyleBytes_[i];
  }
  const size_t textBytes = strlen(text);
  scanText_.insert(insertOffset, text, textBytes);
  scanStyleBytes_[resolvedStyle] += static_cast<uint32_t>(textBytes);
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanText_.clear();
  manager_->scanText_.reserve(2048);  // Pre-allocate to avoid heap fragmentation from repeated concat
  memset(manager_->scanStyleBytes_, 0, sizeof(manager_->scanStyleBytes_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanText_.empty()) return;

  uint32_t styleStart = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const uint32_t styleBytes = manager_->scanStyleBytes_[i];
    if (styleBytes == 0) continue;

    const uint32_t styleEnd = styleStart + styleBytes;
    const bool hasFollowingText = styleEnd < manager_->scanText_.size();
    const char savedByte = hasFollowingText ? manager_->scanText_[styleEnd] : '\0';
    if (hasFollowingText) manager_->scanText_[styleEnd] = '\0';

    manager_->prewarmCache(manager_->scanFontId_, manager_->scanText_.c_str() + styleStart, 1 << i);

    if (hasFollowingText) manager_->scanText_[styleEnd] = savedByte;
    styleStart = styleEnd;
  }

  // Free scan string memory
  manager_->scanText_.clear();
  manager_->scanText_.shrink_to_fit();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanText_ is empty)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
