#include "BookPageMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr uint8_t PAGEMAP_FILE_VERSION = 1;
// Coarse seed used only before any chapter is paginated; replaced by the
// calibrated average as soon as the first section with pages is recorded.
constexpr float DEFAULT_BYTES_PER_PAGE = 2000.0f;
}  // namespace

bool PageMapFingerprint::operator==(const PageMapFingerprint& o) const {
  return fontId == o.fontId && lineCompression == o.lineCompression &&
         extraParagraphSpacing == o.extraParagraphSpacing && paragraphAlignment == o.paragraphAlignment &&
         viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
         hyphenationEnabled == o.hyphenationEnabled && embeddedStyle == o.embeddedStyle &&
         imageRendering == o.imageRendering && focusReadingEnabled == o.focusReadingEnabled;
}

void BookPageMap::init(std::vector<uint32_t> sectionBytes, const PageMapFingerprint& fingerprint) {
  sectionBytes_ = std::move(sectionBytes);
  fingerprint_ = fingerprint;
  pages_.assign(sectionBytes_.size(), UNKNOWN);
}

bool BookPageMap::recordSection(int index, uint16_t pageCount) {
  if (index < 0 || index >= static_cast<int>(pages_.size())) {
    return false;
  }
  const int32_t v = static_cast<int32_t>(pageCount);
  if (pages_[index] == v) {
    return false;
  }
  pages_[index] = v;
  return true;
}

float BookPageMap::bytesPerPage() const {
  uint64_t knownBytes = 0;
  uint32_t knownPages = 0;
  for (size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i] > 0) {  // exclude empty (known-0) chapters from calibration
      knownBytes += sectionBytes_[i];
      knownPages += static_cast<uint32_t>(pages_[i]);
    }
  }
  if (knownPages == 0) {
    return DEFAULT_BYTES_PER_PAGE;
  }
  return static_cast<float>(knownBytes) / static_cast<float>(knownPages);
}

int BookPageMap::pagesOrEstimate(int index, float bpp) const {
  if (pages_[index] >= 0) {
    return pages_[index];  // exact (including a genuine 0)
  }
  if (bpp <= 0.0f) {
    return 1;
  }
  const int est = static_cast<int>(std::lround(static_cast<float>(sectionBytes_[index]) / bpp));
  return std::max(1, est);
}

int BookPageMap::globalPage(int spineIndex, int pageInSection) const {
  const float bpp = bytesPerPage();
  int sum = 0;
  const int n = static_cast<int>(pages_.size());
  for (int i = 0; i < spineIndex && i < n; ++i) {
    sum += pagesOrEstimate(i, bpp);
  }
  return sum + pageInSection + 1;
}

int BookPageMap::total() const {
  const float bpp = bytesPerPage();
  int sum = 0;
  for (size_t i = 0; i < pages_.size(); ++i) {
    sum += pagesOrEstimate(static_cast<int>(i), bpp);
  }
  return sum;
}

bool BookPageMap::isExact() const {
  if (pages_.empty()) {
    return false;
  }
  for (int32_t p : pages_) {
    if (p < 0) {
      return false;
    }
  }
  return true;
}

int BookPageMap::nextUnknown(int fromIndex) const {
  for (int i = std::max(0, fromIndex); i < static_cast<int>(pages_.size()); ++i) {
    if (pages_[i] < 0) {
      return i;
    }
  }
  return -1;
}

bool BookPageMap::save(const std::string& path) const {
  HalFile f;
  if (!Storage.openFileForWrite("PMAP", path, f)) {
    LOG_ERR("PMAP", "Could not open pagemap for write");
    return false;
  }
  serialization::writePod(f, PAGEMAP_FILE_VERSION);
  serialization::writePod(f, fingerprint_.fontId);
  serialization::writePod(f, fingerprint_.lineCompression);
  serialization::writePod(f, fingerprint_.extraParagraphSpacing);
  serialization::writePod(f, fingerprint_.paragraphAlignment);
  serialization::writePod(f, fingerprint_.viewportWidth);
  serialization::writePod(f, fingerprint_.viewportHeight);
  serialization::writePod(f, fingerprint_.hyphenationEnabled);
  serialization::writePod(f, fingerprint_.embeddedStyle);
  serialization::writePod(f, fingerprint_.imageRendering);
  serialization::writePod(f, fingerprint_.focusReadingEnabled);
  serialization::writePod(f, static_cast<uint16_t>(pages_.size()));
  for (int32_t p : pages_) {
    serialization::writePod(f, p);
  }
  return true;
}

bool BookPageMap::load(const std::string& path) {
  // Contract (see header): on any load failure, leave all counts unknown.
  // Reset up front so an early return can never leave stale counts behind if
  // load() is called on a previously populated map.
  pages_.assign(pages_.size(), UNKNOWN);

  HalFile f;
  if (!Storage.openFileForRead("PMAP", path, f)) {
    return false;  // missing - keep all-unknown
  }
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != PAGEMAP_FILE_VERSION) {
    LOG_DBG("PMAP", "Pagemap version mismatch (%u)", version);
    return false;
  }
  PageMapFingerprint fp;
  serialization::readPod(f, fp.fontId);
  serialization::readPod(f, fp.lineCompression);
  serialization::readPod(f, fp.extraParagraphSpacing);
  serialization::readPod(f, fp.paragraphAlignment);
  serialization::readPod(f, fp.viewportWidth);
  serialization::readPod(f, fp.viewportHeight);
  serialization::readPod(f, fp.hyphenationEnabled);
  serialization::readPod(f, fp.embeddedStyle);
  serialization::readPod(f, fp.imageRendering);
  serialization::readPod(f, fp.focusReadingEnabled);
  if (!(fp == fingerprint_)) {
    LOG_DBG("PMAP", "Pagemap fingerprint mismatch; ignoring");
    return false;
  }
  uint16_t count = 0;
  serialization::readPod(f, count);
  if (count != pages_.size()) {
    LOG_DBG("PMAP", "Pagemap spine count mismatch (%u vs %u)", count, static_cast<unsigned>(pages_.size()));
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    int32_t p = UNKNOWN;
    serialization::readPod(f, p);
    pages_[i] = p;
  }
  LOG_DBG("PMAP", "Pagemap loaded: %u sections", static_cast<unsigned>(count));
  return true;
}
