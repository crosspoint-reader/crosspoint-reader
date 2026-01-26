#include "Section.h"

#include <SDCardManager.h>
#include <Serialization.h>

#include <fstream>
#include <set>

#include "FsHelpers.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 10;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) +
                                 sizeof(uint32_t);
}  // namespace

// Helper function to write XML-escaped text directly to file
static bool writeEscapedXml(FsFile& file, const char* text) {
  if (!text) return true;

  // Use a static buffer to avoid heap allocation
  static char buffer[2048];
  int bufferPos = 0;

  while (*text && bufferPos < sizeof(buffer) - 10) {  // Leave margin for entities
    unsigned char c = (unsigned char)*text;

    // Only escape the 5 XML special characters
    if (c == '<') {
      if (bufferPos + 4 < sizeof(buffer)) {
        memcpy(&buffer[bufferPos], "&lt;", 4);
        bufferPos += 4;
      }
    } else if (c == '>') {
      if (bufferPos + 4 < sizeof(buffer)) {
        memcpy(&buffer[bufferPos], "&gt;", 4);
        bufferPos += 4;
      }
    } else if (c == '&') {
      if (bufferPos + 5 < sizeof(buffer)) {
        memcpy(&buffer[bufferPos], "&amp;", 5);
        bufferPos += 5;
      }
    } else if (c == '"') {
      if (bufferPos + 6 < sizeof(buffer)) {
        memcpy(&buffer[bufferPos], "&quot;", 6);
        bufferPos += 6;
      }
    } else if (c == '\'') {
      if (bufferPos + 6 < sizeof(buffer)) {
        memcpy(&buffer[bufferPos], "&apos;", 6);
        bufferPos += 6;
      }
    } else {
      // Keep everything else (include UTF8)
      buffer[bufferPos++] = (char)c;
    }

    text++;
  }

  buffer[bufferPos] = '\0';

  // Write all at once
  size_t written = file.write((const uint8_t*)buffer, bufferPos);
  file.flush();

  return written == bufferPos;
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    Serial.printf("[%lu] [SCT] File not open for writing page %d\n", millis(), pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    Serial.printf("[%lu] [SCT] Failed to serialize page %d\n", millis(), pageCount);
    return 0;
  }
  // Debug reduce log spam
  // Serial.printf("[%lu] [SCT] Page %d processed\n", millis(), pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled) {
  if (!file) {
    Serial.printf("[%lu] [SCT] File not open for writing header\n", millis());
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0 when written)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled) {
  if (!SdMan.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      file.close();
      Serial.printf("[%lu] [SCT] Deserialization failed: Unknown version %u\n", millis(), version);
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
      Serial.printf("[%lu] [SCT] Deserialization failed: Parameters do not match\n", millis());
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  file.close();
  Serial.printf("[%lu] [SCT] Deserialization succeeded: %d pages\n", millis(), pageCount);
  return true;
}

bool Section::clearCache() const {
  if (!SdMan.exists(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!SdMan.remove(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Cache cleared successfully\n", millis());
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled,
                                const std::function<void()>& progressSetupFn,
                                const std::function<void(int)>& progressFn) {
  constexpr uint32_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

  BookMetadataCache::SpineEntry spineEntry = epub->getSpineItem(spineIndex);
  const std::string localPath = spineEntry.href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    SdMan.mkdir(sectionsDir.c_str());
  }

  bool isVirtual = epub->isVirtualSpineItem(spineIndex);
  bool success = false;
  uint32_t fileSize = 0;
  std::string fileToParse = tmpHtmlPath;

  if (isVirtual) {
    Serial.printf("[%lu] [SCT] Processing virtual spine item: %s\n", millis(), localPath.c_str());
    // For virtual items, the path is already on SD, e.g. /sd/cache/...
    // But we need to make sure the parser can read it.
    // If it starts with /sd/, we might need to strip it if using SdFat with root?
    // Assuming absolute path is fine.
    fileToParse = localPath;
    success = true;
    fileSize = 0;  // Don't check size for progress bar on virtual items
  } else {
    // Normal file - stream from zip
    for (int attempt = 0; attempt < 3 && !success; attempt++) {
      if (attempt > 0) delay(50);

      if (SdMan.exists(tmpHtmlPath.c_str())) SdMan.remove(tmpHtmlPath.c_str());

      FsFile tmpHtml;
      if (!SdMan.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) continue;
      success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
      fileSize = tmpHtml.size();
      tmpHtml.close();

      if (!success && SdMan.exists(tmpHtmlPath.c_str())) SdMan.remove(tmpHtmlPath.c_str());
    }

    if (!success) {
      Serial.printf("[%lu] [SCT] Failed to stream item contents\n", millis());
      return false;
    }
  }

  // Only show progress bar for larger chapters
  if (progressSetupFn && fileSize >= MIN_SIZE_FOR_PROGRESS) {
    progressSetupFn();
  }

  if (!SdMan.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled);
  std::vector<uint32_t> lut = {};

  std::unique_ptr<ChapterHtmlSlimParser> visitor(new ChapterHtmlSlimParser(
      fileToParse, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      progressFn));

  Hyphenator::setPreferredLanguage(epub->getLanguage());

  // Track which inline footnotes AND paragraph notes are actually referenced in this file
  std::set<std::string> rewrittenInlineIds;
  int noterefCount = 0;

  visitor->setNoterefCallback([this, &noterefCount, &rewrittenInlineIds](Noteref& noteref) {
    // Extract the ID from the href for tracking
    std::string href(noteref.href);

    // Check if this was rewritten to an inline or paragraph note
    if (href.find("inline_") == 0 || href.find("pnote_") == 0) {
      size_t underscorePos = href.find('_');
      size_t dotPos = href.find('.');

      if (underscorePos != std::string::npos && dotPos != std::string::npos) {
        std::string noteId = href.substr(underscorePos + 1, dotPos - underscorePos - 1);
        rewrittenInlineIds.insert(noteId);
      }
    } else {
      // Normal external footnote
      epub->markAsFootnotePage(noteref.href);
    }
    noterefCount++;
  });

  success = visitor->parseAndBuildPages();

  if (!isVirtual) {
    SdMan.remove(tmpHtmlPath.c_str());
  }

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    file.close();
    SdMan.remove(filePath.c_str());
    return false;
  }

  // --- Footnote Generation Logic (Merged from HEAD) ---

  // Inline footnotes
  for (int i = 0; i < visitor->inlineFootnoteCount; i++) {
    const char* inlineId = visitor->inlineFootnotes[i].id;
    const char* inlineText = visitor->inlineFootnotes[i].text;

    if (rewrittenInlineIds.find(std::string(inlineId)) == rewrittenInlineIds.end()) continue;
    if (!inlineText || strlen(inlineText) == 0) continue;

    char inlineFilename[64];
    snprintf(inlineFilename, sizeof(inlineFilename), "inline_%s.html", inlineId);
    std::string fullPath = epub->getCachePath() + "/" + std::string(inlineFilename);

    FsFile file;
    if (SdMan.openFileForWrite("SCT", fullPath, file)) {
      file.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      file.println("<!DOCTYPE html>");
      file.println("<html xmlns=\"http://www.w3.org/1999/xhtml\">");
      file.println("<head><meta charset=\"UTF-8\"/><title>Footnote</title></head>");
      file.println("<body>");
      file.print("<p id=\"");
      file.print(inlineId);
      file.print("\">");
      writeEscapedXml(file, inlineText);
      file.println("</p></body></html>");
      file.close();

      int virtualIndex = epub->addVirtualSpineItem(fullPath);
      char newHref[128];
      snprintf(newHref, sizeof(newHref), "%s#%s", inlineFilename, inlineId);
      epub->markAsFootnotePage(newHref);
    }
  }

  // Paragraph notes
  for (int i = 0; i < visitor->paragraphNoteCount; i++) {
    const char* pnoteId = visitor->paragraphNotes[i].id;
    const char* pnoteText = visitor->paragraphNotes[i].text;

    if (!pnoteText || strlen(pnoteText) == 0) continue;
    if (rewrittenInlineIds.find(std::string(pnoteId)) == rewrittenInlineIds.end()) continue;

    char pnoteFilename[64];
    snprintf(pnoteFilename, sizeof(pnoteFilename), "pnote_%s.html", pnoteId);
    std::string fullPath = epub->getCachePath() + "/" + std::string(pnoteFilename);

    FsFile file;
    if (SdMan.openFileForWrite("SCT", fullPath, file)) {
      file.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      file.println("<!DOCTYPE html>");
      file.println("<html xmlns=\"http://www.w3.org/1999/xhtml\">");
      file.println("<head><meta charset=\"UTF-8\"/><title>Note</title></head>");
      file.println("<body>");
      file.print("<p id=\"");
      file.print(pnoteId);
      file.print("\">");
      writeEscapedXml(file, pnoteText);
      file.println("</p></body></html>");
      file.close();

      int virtualIndex = epub->addVirtualSpineItem(fullPath);
      char newHref[128];
      snprintf(newHref, sizeof(newHref), "%s#%s", pnoteFilename, pnoteId);
      epub->markAsFootnotePage(newHref);
    }
  }

  // Write LUT (master)
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
    Serial.printf("[%lu] [SCT] Failed to write LUT due to invalid page positions\n", millis());
    file.close();
    SdMan.remove(filePath.c_str());
    return false;
  }

  // Go back and write LUT offset
  file.seek(HEADER_SIZE - sizeof(uint32_t) - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  file.close();
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!SdMan.openFileForRead("SCT", filePath, file)) {
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
