#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

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
    if (!data || !data->groups) continue;
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

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  ScanRecord* record = nullptr;
  for (size_t i = 0; i < scanRecordCount_; i++) {
    if (scanRecords_[i].fontId == fontId) {
      record = &scanRecords_[i];
      break;
    }
  }
  if (!record) {
    if (scanRecordCount_ >= MAX_SCAN_RECORDS) return;  // overflow fonts fall back to on-demand loading
    record = &scanRecords_[scanRecordCount_++];
    record->fontId = fontId;
    // First record is the page body text (largest); later ones are short UI
    // strings like the status-bar title.
    record->text.reserve(scanRecordCount_ == 1 ? 2048 : 256);
  }
  record->text += text;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  record->styleCounts[baseStyle] += cpCount;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  for (size_t i = 0; i < manager_->scanRecordCount_; i++) {
    manager_->scanRecords_[i] = ScanRecord{};
  }
  manager_->scanRecordCount_ = 0;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;

  for (size_t i = 0; i < manager_->scanRecordCount_; i++) {
    ScanRecord& record = manager_->scanRecords_[i];
    if (record.text.empty()) continue;

    // Build style bitmask from all styles that appeared during the scan
    uint8_t styleMask = 0;
    for (uint8_t s = 0; s < 4; s++) {
      if (record.styleCounts[s] > 0) styleMask |= (1 << s);
    }
    if (styleMask == 0) styleMask = 1;  // default to regular

    manager_->prewarmCache(record.fontId, record.text.c_str(), styleMask);

    // Free scan string memory
    record = ScanRecord{};
  }
  manager_->scanRecordCount_ = 0;
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
