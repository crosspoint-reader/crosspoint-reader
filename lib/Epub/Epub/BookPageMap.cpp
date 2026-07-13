#include "BookPageMap.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr uint8_t PAGEMAP_FILE_VERSION = 2;
// Coarse seed used only before any chapter is paginated; replaced by the
// calibrated average as soon as the first section with pages is recorded.
constexpr double DEFAULT_BYTES_PER_PAGE = 2000.0;

constexpr size_t PAGEMAP_HEADER_SIZE = sizeof(uint8_t) +  // file version
                                       sizeof(uint8_t) +  // section cache version
                                       sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                       2 * sizeof(uint16_t) + 2 * sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                       sizeof(uint16_t);

template <typename T>
bool writePodExact(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
bool readPodExact(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

int addPagesSaturated(const int total, const int pages) {
  if (pages > std::numeric_limits<int>::max() - total) {
    return std::numeric_limits<int>::max();
  }
  return total + pages;
}
}  // namespace

bool PageMapFingerprint::operator==(const PageMapFingerprint& o) const {
  return sectionCacheVersion == o.sectionCacheVersion && fontId == o.fontId && lineCompression == o.lineCompression &&
         extraParagraphSpacing == o.extraParagraphSpacing && paragraphAlignment == o.paragraphAlignment &&
         viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
         hyphenationEnabled == o.hyphenationEnabled && embeddedStyle == o.embeddedStyle &&
         imageRendering == o.imageRendering && focusReadingEnabled == o.focusReadingEnabled;
}

bool BookPageMap::init(const int sectionCount, const PageMapFingerprint& fingerprint) {
  sections_.reset();
  sectionCount_ = 0;
  fingerprint_ = fingerprint;
  if (sectionCount < 0 || sectionCount > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  if (sectionCount == 0) {
    return true;
  }
  auto sections = makeUniqueNoThrow<SectionEntry[]>(static_cast<size_t>(sectionCount));
  if (!sections) {
    return false;
  }
  sections_ = std::move(sections);
  sectionCount_ = sectionCount;
  resetCounts();
  return true;
}

void BookPageMap::setSectionBytes(const int index, const uint32_t sectionBytes) {
  if (index >= 0 && index < sectionCount_) {
    sections_[index].bytes = sectionBytes;
  }
}

void BookPageMap::resetCounts() {
  for (int i = 0; i < sectionCount_; ++i) {
    sections_[i].pages = UNKNOWN;
  }
}

bool BookPageMap::recordSection(int index, uint16_t pageCount) {
  if (index < 0 || index >= sectionCount_) {
    return false;
  }
  const int32_t v = static_cast<int32_t>(pageCount);
  if (sections_[index].pages == v) {
    return false;
  }
  sections_[index].pages = v;
  return true;
}

double BookPageMap::bytesPerPage(const int estimatedSectionIndex, const int estimatedSectionPages) const {
  uint64_t knownBytes = 0;
  uint64_t knownPages = 0;
  for (int i = 0; i < sectionCount_; ++i) {
    if (sections_[i].pages > 0) {  // exclude empty (known-0) chapters from calibration
      knownBytes += sections_[i].bytes;
      knownPages += static_cast<uint32_t>(sections_[i].pages);
    }
  }

  const bool useLiveEstimate = estimatedSectionIndex >= 0 && estimatedSectionIndex < sectionCount_ &&
                               sections_[estimatedSectionIndex].pages == UNKNOWN && estimatedSectionPages > 0 &&
                               sections_[estimatedSectionIndex].bytes > 0;
  if (useLiveEstimate) {
    knownBytes += sections_[estimatedSectionIndex].bytes;
    knownPages += static_cast<uint32_t>(estimatedSectionPages);
  }

  if (knownPages == 0 || knownBytes == 0) {
    return DEFAULT_BYTES_PER_PAGE;
  }
  return static_cast<double>(knownBytes) / static_cast<double>(knownPages);
}

int BookPageMap::pagesOrEstimate(const int index, const double bpp, const int estimatedSectionIndex,
                                 const int estimatedSectionPages) const {
  if (sections_[index].pages >= 0) {
    return sections_[index].pages;  // exact (including a genuine 0)
  }
  if (index == estimatedSectionIndex && estimatedSectionPages > 0) {
    return estimatedSectionPages;
  }
  if (bpp <= 0.0) {
    return 1;
  }
  const double rawEstimate = static_cast<double>(sections_[index].bytes) / bpp;
  if (!std::isfinite(rawEstimate) || rawEstimate >= std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  const int estimate = static_cast<int>(std::floor(rawEstimate + 0.5));
  return std::max(1, estimate);
}

int BookPageMap::globalPage(const int spineIndex, const int pageInSection, const int currentSectionEstimate) const {
  const double bpp = bytesPerPage(spineIndex, currentSectionEstimate);
  int sum = 0;
  for (int i = 0; i < spineIndex && i < sectionCount_; ++i) {
    sum = addPagesSaturated(sum, pagesOrEstimate(i, bpp, spineIndex, currentSectionEstimate));
  }
  const int pageOffset = pageInSection >= std::numeric_limits<int>::max() - 1 ? std::numeric_limits<int>::max()
                                                                              : std::max(0, pageInSection) + 1;
  return addPagesSaturated(sum, pageOffset);
}

int BookPageMap::total(const int estimatedSectionIndex, const int estimatedSectionPages) const {
  const double bpp = bytesPerPage(estimatedSectionIndex, estimatedSectionPages);
  int sum = 0;
  for (int i = 0; i < sectionCount_; ++i) {
    sum = addPagesSaturated(sum, pagesOrEstimate(i, bpp, estimatedSectionIndex, estimatedSectionPages));
  }
  return sum;
}

bool BookPageMap::isExact() const {
  if (sectionCount_ == 0) {
    return false;
  }
  for (int i = 0; i < sectionCount_; ++i) {
    if (sections_[i].pages < 0) {
      return false;
    }
  }
  return true;
}

bool BookPageMap::save(const std::string& path) const {
  const std::string tmpPath = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("PMAP", tmpPath, f)) {
      LOG_ERR("PMAP", "Could not open temporary pagemap for write");
      return false;
    }

    bool written = writePodExact(f, PAGEMAP_FILE_VERSION) && writePodExact(f, fingerprint_.sectionCacheVersion) &&
                   writePodExact(f, fingerprint_.fontId) && writePodExact(f, fingerprint_.lineCompression) &&
                   writePodExact(f, fingerprint_.extraParagraphSpacing) &&
                   writePodExact(f, fingerprint_.paragraphAlignment) && writePodExact(f, fingerprint_.viewportWidth) &&
                   writePodExact(f, fingerprint_.viewportHeight) && writePodExact(f, fingerprint_.hyphenationEnabled) &&
                   writePodExact(f, fingerprint_.embeddedStyle) && writePodExact(f, fingerprint_.imageRendering) &&
                   writePodExact(f, fingerprint_.focusReadingEnabled) &&
                   writePodExact(f, static_cast<uint16_t>(sectionCount_));
    for (int i = 0; written && i < sectionCount_; ++i) {
      written = writePodExact(f, sections_[i].pages);
    }
    if (!written) {
      LOG_ERR("PMAP", "Short write saving pagemap");
      f.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    f.flush();
    f.close();
  }

  // SdFat rename does not replace an existing destination. Losing this derived
  // cache between remove and rename is safe; accepting a torn canonical file is not.
  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("PMAP", "Could not promote temporary pagemap");
    return false;
  }
  return true;
}

bool BookPageMap::load(const std::string& path) {
  // Contract (see header): on any load failure, leave all counts unknown.
  // Reset up front so an early return can never leave stale counts behind if
  // load() is called on a previously populated map.
  resetCounts();

  HalFile f;
  if (!Storage.openFileForRead("PMAP", path, f)) {
    return false;  // missing - keep all-unknown
  }
  const size_t expectedSize = PAGEMAP_HEADER_SIZE + static_cast<size_t>(sectionCount_) * sizeof(int32_t);
  if (f.size() != expectedSize) {
    LOG_DBG("PMAP", "Pagemap size mismatch (%u vs %u)", static_cast<unsigned>(f.size()),
            static_cast<unsigned>(expectedSize));
    return false;
  }

  uint8_t version = 0;
  if (!readPodExact(f, version) || version != PAGEMAP_FILE_VERSION) {
    LOG_DBG("PMAP", "Pagemap version mismatch (%u)", version);
    return false;
  }
  PageMapFingerprint fp;
  const bool fingerprintRead = readPodExact(f, fp.sectionCacheVersion) && readPodExact(f, fp.fontId) &&
                               readPodExact(f, fp.lineCompression) && readPodExact(f, fp.extraParagraphSpacing) &&
                               readPodExact(f, fp.paragraphAlignment) && readPodExact(f, fp.viewportWidth) &&
                               readPodExact(f, fp.viewportHeight) && readPodExact(f, fp.hyphenationEnabled) &&
                               readPodExact(f, fp.embeddedStyle) && readPodExact(f, fp.imageRendering) &&
                               readPodExact(f, fp.focusReadingEnabled);
  if (!fingerprintRead) {
    LOG_DBG("PMAP", "Pagemap fingerprint is truncated");
    return false;
  }
  if (!(fp == fingerprint_)) {
    LOG_DBG("PMAP", "Pagemap fingerprint mismatch; ignoring");
    return false;
  }
  uint16_t count = 0;
  if (!readPodExact(f, count) || count != sectionCount_) {
    LOG_DBG("PMAP", "Pagemap spine count mismatch (%u vs %u)", count, static_cast<unsigned>(sectionCount_));
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    int32_t p = UNKNOWN;
    if (!readPodExact(f, p) || p < UNKNOWN || p > std::numeric_limits<uint16_t>::max()) {
      LOG_DBG("PMAP", "Pagemap page count is invalid at section %u", static_cast<unsigned>(i));
      resetCounts();
      return false;
    }
    sections_[i].pages = p;
  }
  LOG_DBG("PMAP", "Pagemap loaded: %u sections", static_cast<unsigned>(count));
  return true;
}
