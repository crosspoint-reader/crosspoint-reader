#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

  // Constructor for non-EPUB use (markdown, etc.) — takes section file path directly
  explicit Section(const std::string& sectionFilePath, GfxRenderer& renderer)
      : epub(nullptr), spineIndex(0), renderer(renderer), filePath(sectionFilePath) {}

  ~Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Create a section file from a standalone HTML file (no EPUB context)
  static bool createFromHtmlFile(const std::string& htmlPath, const std::string& sectionFilePath,
                                 GfxRenderer& renderer, int fontId, float lineCompression, bool extraParagraphSpacing,
                                 uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                 bool hyphenationEnabled, const std::function<void()>& popupFn,
                                 uint16_t& outPageCount);
};
