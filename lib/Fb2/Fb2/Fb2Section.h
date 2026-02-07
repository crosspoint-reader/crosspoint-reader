#pragma once
#include <SdFat.h>

#include <functional>
#include <memory>
#include <string>

#include "Fb2.h"

class Page;
class GfxRenderer;

class Fb2Section {
  std::shared_ptr<Fb2> fb2;
  const int sectionIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Fb2Section(const std::shared_ptr<Fb2>& fb2, const int sectionIndex, GfxRenderer& renderer)
      : fb2(fb2),
        sectionIndex(sectionIndex),
        renderer(renderer),
        filePath(fb2->getCachePath() + "/sections/" + std::to_string(sectionIndex) + ".bin") {}
  ~Fb2Section() = default;
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();
};
