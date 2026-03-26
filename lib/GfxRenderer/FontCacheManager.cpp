#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>

#include <cstring>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap) : fontMap_(fontMap) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  if (!fontDecompressor_) return;
  // Avoid std::map::at(): with -fno-exceptions, out-of-range can abort().
  const auto itFont = fontMap_.find(fontId);
  if (itFont == fontMap_.end()) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = itFont->second.getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  // PDF (and huge EPUB sections) can exceed available DRAM if we concatenate full extracted text for prewarm.
  // With -fno-exceptions, operator new failure terminates the process.
  static constexpr size_t kMaxScanTextBytes = 32 * 1024;
  size_t appendLen = 0;
  if (text && *text && scanText_.size() < kMaxScanTextBytes) {
    const size_t room = kMaxScanTextBytes - scanText_.size();
    appendLen = strnlen(text, room);
    if (appendLen > 0) {
      scanText_.append(text, appendLen);
    }
  }
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  if (appendLen > 0) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    uint32_t cpCount = 0;
    const auto* end = p + appendLen;
    while (p < end) {
      if ((*p & 0xC0) != 0x80) cpCount++;
      p++;
    }
    scanStyleCounts_[baseStyle] += cpCount;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanText_.clear();
  manager_->scanText_.reserve(2048);  // Pre-allocate to avoid heap fragmentation from repeated concat
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanText_.empty()) return;

  // Build style bitmask from all styles that appeared during the scan
  uint8_t styleMask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanStyleCounts_[i] > 0) styleMask |= (1 << i);
  }
  if (styleMask == 0) styleMask = 1;  // default to regular

  manager_->prewarmCache(manager_->scanFontId_, manager_->scanText_.c_str(), styleMask);

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
