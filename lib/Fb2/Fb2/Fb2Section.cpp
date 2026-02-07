#include "Fb2Section.h"

#include <Epub/Page.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <HalStorage.h>
#include <Serialization.h>

#include "Fb2SectionParser.h"

namespace {
// FB2 section files don't track embeddedStyle (no CSS in FB2).
// We use a separate version from EPUB sections.
constexpr uint8_t FB2_SECTION_FILE_VERSION = 1;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) +
                                 sizeof(uint32_t);
}  // namespace

uint32_t Fb2Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    Serial.printf("[%lu] [FBS] File not open for writing page %d\n", millis(), pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    Serial.printf("[%lu] [FBS] Failed to serialize page %d\n", millis(), pageCount);
    return 0;
  }
  Serial.printf("[%lu] [FBS] Page %d processed\n", millis(), pageCount);

  pageCount++;
  return position;
}

void Fb2Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                        const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                        const uint16_t viewportHeight, const bool hyphenationEnabled) {
  if (!file) {
    Serial.printf("[%lu] [FBS] File not open for writing header\n", millis());
    return;
  }
  serialization::writePod(file, FB2_SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, pageCount);                 // Placeholder
  serialization::writePod(file, static_cast<uint32_t>(0));  // LUT offset placeholder
}

bool Fb2Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled) {
  if (!Storage.openFileForRead("FBS", filePath, file)) {
    return false;
  }

  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != FB2_SECTION_FILE_VERSION) {
      file.close();
      Serial.printf("[%lu] [FBS] Version mismatch: %u\n", millis(), version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled) {
      file.close();
      Serial.printf("[%lu] [FBS] Parameters do not match\n", millis());
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  file.close();
  Serial.printf("[%lu] [FBS] Loaded section: %d pages\n", millis(), pageCount);
  return true;
}

bool Fb2Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    Serial.printf("[%lu] [FBS] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [FBS] Cache cleared\n", millis());
  return true;
}

bool Fb2Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                   const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                   const uint16_t viewportHeight, const bool hyphenationEnabled,
                                   const std::function<void()>& popupFn) {
  const auto& sectionInfo = fb2->getSectionInfo(sectionIndex);

  // Create cache directory
  {
    const auto sectionsDir = fb2->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  if (!Storage.openFileForWrite("FBS", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled);
  std::vector<uint32_t> lut = {};

  // If there's only one section with fileOffset 0, the metadata parser found no real <section> tags.
  // Pass -1 to tell the parser to process all body content instead of filtering by section index.
  const int targetIndex = (fb2->getSectionCount() == 1 && sectionInfo.fileOffset == 0) ? -1 : sectionIndex;
  Fb2SectionParser visitor(
      fb2->getPath(), sectionInfo.fileOffset, sectionInfo.length, targetIndex, renderer, fontId, lineCompression,
      extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); }, popupFn);
  Hyphenator::setPreferredLanguage(fb2->getLanguage());
  bool success = visitor.parseAndBuildPages();

  if (!success) {
    Serial.printf("[%lu] [FBS] Failed to parse and build pages\n", millis());
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    Serial.printf("[%lu] [FBS] Failed LUT records\n", millis());
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write final page count and LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();
  return true;
}

std::unique_ptr<Page> Fb2Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("FBS", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  file.close();
  return page;
}
