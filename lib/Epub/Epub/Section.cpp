#include "Section.h"

#include <HalStorage.h>
#include <Serialization.h>

#include <fstream>
#include <set>

#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// Version 13: Page now includes footnote serialization (number + href)
constexpr uint8_t SECTION_FILE_VERSION = 13;

constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint32_t);
}  // namespace

// Helper function to write XML-escaped text directly to file
static bool writeEscapedXml(FsFile& file, const char* text) {
  if (!text) return true;

  char buffer[2048];
  size_t bufferPos = 0;

  // Helper to flush current buffer contents to the file
  auto flushBuffer = [&]() -> bool {
    if (bufferPos == 0) return true;

    size_t totalWritten = 0;
    while (totalWritten < bufferPos) {
      const size_t written =
          file.write(reinterpret_cast<const uint8_t*>(buffer) + totalWritten, bufferPos - totalWritten);
      if (written == 0) {
        return false;  // Write failed
      }
      totalWritten += written;
    }
    bufferPos = 0;
    return true;
  };

  while (*text) {
    const unsigned char c = static_cast<unsigned char>(*text);
    const char* entity = nullptr;
    size_t entityLen = 0;

    // Only escape the 5 XML special characters
    switch (c) {
      case '<':
        entity = "&lt;";
        entityLen = 4;
        break;
      case '>':
        entity = "&gt;";
        entityLen = 4;
        break;
      case '&':
        entity = "&amp;";
        entityLen = 5;
        break;
      case '"':
        entity = "&quot;";
        entityLen = 6;
        break;
      case '\'':
        entity = "&apos;";
        entityLen = 6;
        break;
    }

    if (entity) {
      // Ensure there is enough space for the entity, flushing if necessary
      if (bufferPos + entityLen > sizeof(buffer)) {
        if (!flushBuffer()) return false;
      }
      memcpy(&buffer[bufferPos], entity, entityLen);
      bufferPos += entityLen;
    } else {
      // Keep everything else (including UTF-8 bytes)
      if (bufferPos + 1 > sizeof(buffer)) {
        if (!flushBuffer()) return false;
      }
      buffer[bufferPos++] = static_cast<char>(c);
    }

    ++text;
  }

  // Flush any remaining data in the buffer
  return flushBuffer();
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
  Serial.printf("[%lu] [SCT] Page %d processed\n", millis(), pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle) {
  if (!file) {
    Serial.printf("[%lu] [SCT] File not open for writing header\n", millis());
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0 when written)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
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
    bool fileEmbeddedStyle;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle) {
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

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Cache does not exist, no action needed\n", millis());
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    Serial.printf("[%lu] [SCT] Failed to clear cache\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SCT] Cache cleared successfully\n", millis());
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
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
      if (attempt > 0) {
        Serial.printf("[%lu] [SCT] Retrying stream (attempt %d)...\n", millis(), attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      FsFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
      fileSize = tmpHtml.size();
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!success && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        Serial.printf("[%lu] [SCT] Removed incomplete temp file after failed attempt\n", millis());
      }
    }

    if (!success) {
      Serial.printf("[%lu] [SCT] Failed to stream item contents to temp file after retries\n", millis());
      return false;
    }

    Serial.printf("[%lu] [SCT] Streamed temp HTML to %s (%d bytes)\n", millis(), tmpHtmlPath.c_str(), fileSize);
  }

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle);
  std::vector<uint32_t> lut = {};

  std::unique_ptr<ChapterHtmlSlimParser> visitor(new ChapterHtmlSlimParser(
      fileToParse, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled,
      [this, &lut](std::unique_ptr<Page> page) { lut.emplace_back(this->onPageComplete(std::move(page))); },
      embeddedStyle, popupFn, embeddedStyle ? epub->getCssParser() : nullptr));

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

  Storage.remove(tmpHtmlPath.c_str());

  if (!success) {
    Serial.printf("[%lu] [SCT] Failed to parse XML and build pages\n", millis());
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // --- Footnote Generation Logic ---

  // Inline footnotes
  for (int i = 0; i < visitor->inlineFootnoteCount; i++) {
    const char* inlineId = visitor->inlineFootnotes[i].id;
    const char* inlineText = visitor->inlineFootnotes[i].text;

    if (rewrittenInlineIds.find(std::string(inlineId)) == rewrittenInlineIds.end()) continue;
    if (!inlineText || strlen(inlineText) == 0) continue;

    char inlineFilename[64];
    snprintf(inlineFilename, sizeof(inlineFilename), "inline_%s.html", inlineId);
    std::string fullPath = epub->getCachePath() + "/" + std::string(inlineFilename);

    FsFile inlineFile;  // Different name - doesn't shadow this->file
    if (Storage.openFileForWrite("SCT", fullPath, inlineFile)) {
      inlineFile.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      inlineFile.println("<!DOCTYPE html>");
      inlineFile.println("<html xmlns=\"http://www.w3.org/1999/xhtml\">");
      inlineFile.println("<head><meta charset=\"UTF-8\"/><title>Footnote</title></head>");
      inlineFile.println("<body>");
      inlineFile.print("<p id=\"");
      inlineFile.print(inlineId);
      inlineFile.print("\">");
      writeEscapedXml(inlineFile, inlineText);
      inlineFile.println("</p></body></html>");
      inlineFile.close();

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

    FsFile pnoteFile;  // Different name - doesn't shadow this->file
    if (Storage.openFileForWrite("SCT", fullPath, pnoteFile)) {
      pnoteFile.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      pnoteFile.println("<!DOCTYPE html>");
      pnoteFile.println("<html xmlns=\"http://www.w3.org/1999/xhtml\">");
      pnoteFile.println("<head><meta charset=\"UTF-8\"/><title>Note</title></head>");
      pnoteFile.println("<body>");
      pnoteFile.print("<p id=\"");
      pnoteFile.print(pnoteId);
      pnoteFile.print("\">");
      writeEscapedXml(pnoteFile, pnoteText);
      pnoteFile.println("</p></body></html>");
      pnoteFile.close();

      int virtualIndex = epub->addVirtualSpineItem(fullPath);
      char newHref[128];
      snprintf(newHref, sizeof(newHref), "%s#%s", pnoteFilename, pnoteId);
      epub->markAsFootnotePage(newHref);
    }
  }

  // Continue with LUT writing (file is still open)
  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
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
    Storage.remove(filePath.c_str());
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
  if (!Storage.openFileForRead("SCT", filePath, file)) {
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