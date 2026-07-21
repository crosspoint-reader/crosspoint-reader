#pragma once

#include <cstddef>
#include <cstdint>

namespace SectionCacheValidation {

constexpr uint16_t MAX_PAGE_ELEMENTS = 512;
constexpr uint16_t MAX_TEXT_BLOCK_WORDS = 10000;
constexpr uint32_t MAX_IMAGE_PATH_BYTES = 1024;
constexpr uint32_t MAX_ANCHOR_BYTES = 4096;
constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;
constexpr uint64_t FOOTNOTE_BYTES = 32 + 96;

class Input {
 public:
  virtual ~Input() = default;
  virtual uint64_t size() const = 0;
  virtual bool readAt(uint64_t offset, void* destination, size_t length) = 0;
};

struct Layout {
  uint8_t version = 0;
  bool partial = false;
  uint16_t pageCount = 0;
  uint32_t pageLutOffset = 0;
  uint32_t anchorMapOffset = 0;
  uint32_t paragraphLutOffset = 0;
  uint32_t listItemLutOffset = 0;
  uint32_t partialBytesConsumed = 0;
  uint32_t partialTotalBytes = 0;
  uint64_t headerSize = 0;
  uint64_t fileSize = 0;
};

// Performs an allocation-free validation of the complete cache structure:
// header values, every serialized page, page offsets, anchors, lookup tables,
// and the optional partial-build trailer.
bool validate(Input& input, uint64_t headerSize, uint8_t finalizedVersion, uint8_t partialVersion, Layout& layout);

// Re-reads the selected page LUT entries and returns a bounded serialized page
// range. This remains safe if the file changed after the initial validation.
bool pageBounds(Input& input, const Layout& layout, uint16_t page, uint64_t& begin, uint64_t& end);

}  // namespace SectionCacheValidation
