#pragma once
#include <HalStorage.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Page;

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

/**
 * SectionCache — format-agnostic binary cache for paginated content.
 *
 * Handles reading and writing the section .bin files on the SD card.
 * This class knows nothing about EPUB, HTML, or CSS; any format parser
 * can use it to persist and retrieve laid-out Page objects.
 *
 * Binary layout (all fields little-endian):
 *   [Header]  version + render params + placeholders for counts/offsets
 *   [Pages]   serialized Page objects, back-to-back
 *   [LUT]     uint32_t file offsets for each page
 *   [Anchors] uint16_t count + (string key, uint16_t page) entries
 *   [ParagraphLUT] uint16_t count + uint16_t paragraphIndex per page
 *   [LiLUT]   uint16_t listItemIndex per page (count shared with ParagraphLUT)
 */
class SectionCache {
 public:
  static constexpr uint8_t FILE_VERSION = 23;

 private:
  std::string filePath;
  FsFile file;

  static constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) +
                                          sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                          sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                          sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);

 public:
  uint16_t pageCount = 0;

  explicit SectionCache(std::string filePath) : filePath(std::move(filePath)) {}
  ~SectionCache() = default;

  const std::string& getFilePath() const { return filePath; }

  void writeHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                   uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                   uint8_t imageRendering, bool focusReadingEnabled);

  bool openForWrite();

  uint32_t writePage(std::unique_ptr<Page> page);

  bool finalize(const std::vector<PageLutEntry>& lut, const std::vector<std::pair<std::string, uint16_t>>& anchors);

  bool loadHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                  uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                  uint8_t imageRendering, bool focusReadingEnabled);

  std::unique_ptr<Page> loadPage(int pageIndex);

  bool clearCache() const;

  void closeAndRemove();

  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
