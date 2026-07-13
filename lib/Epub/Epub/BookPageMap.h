#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Render-settings fingerprint. These are exactly the fields Section stores in
// its cache header (see Section.cpp). Page counts are only valid for one
// fingerprint; when any field changes the section caches and this map are
// invalidated together.
struct PageMapFingerprint {
  uint8_t sectionCacheVersion = 0;
  int fontId = 0;
  float lineCompression = 0.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;

  bool operator==(const PageMapFingerprint& o) const;
};

// Tracks per-section (spine item) page counts for a whole book so the reader can
// show book-global "page X of Y". Sections that have been paginated hold an exact
// count; the rest are estimated from their byte size, calibrated against the
// paginated ones. Renderer-/settings-independent: the reader passes byte sizes
// and the fingerprint in.
class BookPageMap {
 public:
  // Allocate storage for sectionCount spine items and reset every count to
  // unknown. Returns false instead of aborting when memory is unavailable.
  bool init(int sectionCount, const PageMapFingerprint& fingerprint);

  // Set the uncompressed XHTML byte size used to estimate an unknown section.
  void setSectionBytes(int index, uint32_t sectionBytes);

  // Mark a section's exact page count (0 is a valid count: an empty chapter).
  // Returns true if the stored value changed.
  bool recordSection(int index, uint16_t pageCount);

  // Book-global current page (1-based) = sum of counts of sections before
  // spineIndex (exact or estimated) + pageInSection + 1. currentSectionEstimate
  // is Section's transient lazy-build estimate; it improves calibration but is
  // never stored as an exact count.
  int globalPage(int spineIndex, int pageInSection, int currentSectionEstimate = 0) const;

  // Book-global total pages (exact + estimated). When estimatedSectionIndex is
  // still unknown, use estimatedSectionPages for that section and to calibrate
  // the remaining byte estimates. An exact stored count always wins.
  int total(int estimatedSectionIndex = -1, int estimatedSectionPages = 0) const;

  // True once every section has an exact count.
  bool isExact() const;

  // Persistence via HalStorage. load() returns false and leaves all counts
  // unknown if the file is missing, the version/fingerprint/spine-count differ.
  bool load(const std::string& path);
  bool save(const std::string& path) const;

  const PageMapFingerprint& fingerprint() const { return fingerprint_; }
  int sectionCount() const { return sectionCount_; }

 private:
  static constexpr int32_t UNKNOWN = -1;

  struct SectionEntry {
    uint32_t bytes = 0;
    int32_t pages = UNKNOWN;  // UNKNOWN(-1), or exact page count (>= 0)
  };

  std::unique_ptr<SectionEntry[]> sections_;
  int sectionCount_ = 0;
  PageMapFingerprint fingerprint_;

  void resetCounts();
  // Average bytes-per-page from exact sections and, when supplied, the current
  // lazy section estimate. Falls back to a coarse seed before either is known.
  double bytesPerPage(int estimatedSectionIndex, int estimatedSectionPages) const;
  // Exact count if known, then the supplied live estimate for its section,
  // otherwise a byte estimate (>= 1).
  int pagesOrEstimate(int index, double bytesPerPage, int estimatedSectionIndex, int estimatedSectionPages) const;
};
