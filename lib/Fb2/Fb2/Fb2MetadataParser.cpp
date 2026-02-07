#include "Fb2MetadataParser.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <expat.h>

#include <cstring>

namespace {
// Strip namespace prefix from tag name (e.g., "l:title" -> "title")
const char* stripNs(const char* name) {
  const char* colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

// Extract attribute value from xlink:href="#id" -> "id"
std::string getXlinkHref(const char** atts) {
  if (!atts) return "";
  for (int i = 0; atts[i]; i += 2) {
    const char* attrName = stripNs(atts[i]);
    if (strcmp(attrName, "href") == 0) {
      const char* val = atts[i + 1];
      if (val[0] == '#') return std::string(val + 1);
      return std::string(val);
    }
  }
  return "";
}
}  // namespace

void Fb2MetadataParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<Fb2MetadataParser*>(userData);
  const char* tag = stripNs(name);

  if (strcmp(tag, "title-info") == 0 && !self->inBody) {
    self->inTitleInfo = true;
    return;
  }

  if (self->inTitleInfo) {
    if (strcmp(tag, "book-title") == 0) {
      self->context = Context::BOOK_TITLE;
      self->charBuffer.clear();
    } else if (strcmp(tag, "author") == 0) {
      self->inAuthor = true;
      self->authorFirstName.clear();
      self->authorMiddleName.clear();
      self->authorLastName.clear();
    } else if (self->inAuthor && strcmp(tag, "first-name") == 0) {
      self->context = Context::AUTHOR_FIRST_NAME;
      self->charBuffer.clear();
    } else if (self->inAuthor && strcmp(tag, "middle-name") == 0) {
      self->context = Context::AUTHOR_MIDDLE_NAME;
      self->charBuffer.clear();
    } else if (self->inAuthor && strcmp(tag, "last-name") == 0) {
      self->context = Context::AUTHOR_LAST_NAME;
      self->charBuffer.clear();
    } else if (strcmp(tag, "lang") == 0) {
      self->context = Context::LANG;
      self->charBuffer.clear();
    } else if (strcmp(tag, "coverpage") == 0) {
      self->context = Context::COVERPAGE;
    } else if (self->context == Context::COVERPAGE && strcmp(tag, "image") == 0) {
      self->coverBinaryId = getXlinkHref(atts);
    }
    return;
  }

  if (strcmp(tag, "body") == 0 && !self->inBody) {
    self->inBody = true;
    self->bodyDepth = 0;
    // Record byte offset of body start
    self->previousSectionEnd = XML_GetCurrentByteIndex(static_cast<XML_Parser>(self->parser));
    return;
  }

  if (self->inBody) {
    if (strcmp(tag, "section") == 0) {
      self->sectionDepth++;
      // Only track top-level sections (depth 1 within body)
      if (self->sectionDepth == 1) {
        self->currentSectionOffset = XML_GetCurrentByteIndex(static_cast<XML_Parser>(self->parser));
        self->currentSectionTitle.clear();
        self->inSectionTitle = false;
      }
    } else if (strcmp(tag, "title") == 0 && self->sectionDepth == 1) {
      self->inSectionTitle = true;
      self->charBuffer.clear();
    } else if (strcmp(tag, "p") == 0 && self->inSectionTitle) {
      self->context = Context::SECTION_TITLE_P;
      self->charBuffer.clear();
    }
  }
}

void Fb2MetadataParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<Fb2MetadataParser*>(userData);
  const char* tag = stripNs(name);

  if (strcmp(tag, "title-info") == 0 && self->inTitleInfo) {
    self->inTitleInfo = false;
    self->context = Context::NONE;
    return;
  }

  if (self->inTitleInfo) {
    if (strcmp(tag, "book-title") == 0 && self->context == Context::BOOK_TITLE) {
      self->title = self->charBuffer;
      self->context = Context::NONE;
    } else if (strcmp(tag, "author") == 0 && self->inAuthor) {
      self->inAuthor = false;
      // Build author string: "First Middle Last"
      std::string fullAuthor;
      if (!self->authorFirstName.empty()) {
        fullAuthor = self->authorFirstName;
      }
      if (!self->authorMiddleName.empty()) {
        if (!fullAuthor.empty()) fullAuthor += " ";
        fullAuthor += self->authorMiddleName;
      }
      if (!self->authorLastName.empty()) {
        if (!fullAuthor.empty()) fullAuthor += " ";
        fullAuthor += self->authorLastName;
      }
      if (!fullAuthor.empty() && self->author.empty()) {
        self->author = fullAuthor;
      }
      self->context = Context::NONE;
    } else if (self->inAuthor && strcmp(tag, "first-name") == 0 && self->context == Context::AUTHOR_FIRST_NAME) {
      self->authorFirstName = self->charBuffer;
      self->context = Context::NONE;
    } else if (self->inAuthor && strcmp(tag, "middle-name") == 0 && self->context == Context::AUTHOR_MIDDLE_NAME) {
      self->authorMiddleName = self->charBuffer;
      self->context = Context::NONE;
    } else if (self->inAuthor && strcmp(tag, "last-name") == 0 && self->context == Context::AUTHOR_LAST_NAME) {
      self->authorLastName = self->charBuffer;
      self->context = Context::NONE;
    } else if (strcmp(tag, "lang") == 0 && self->context == Context::LANG) {
      self->language = self->charBuffer;
      self->context = Context::NONE;
    } else if (strcmp(tag, "coverpage") == 0) {
      self->context = Context::NONE;
    }
    return;
  }

  if (self->inBody) {
    if (strcmp(tag, "title") == 0 && self->inSectionTitle && self->sectionDepth == 1) {
      self->inSectionTitle = false;
      self->context = Context::NONE;
    } else if (strcmp(tag, "p") == 0 && self->context == Context::SECTION_TITLE_P) {
      // Append paragraph text to section title
      if (!self->currentSectionTitle.empty() && !self->charBuffer.empty()) {
        self->currentSectionTitle += " ";
      }
      self->currentSectionTitle += self->charBuffer;
      self->charBuffer.clear();
      self->context = Context::NONE;
    } else if (strcmp(tag, "section") == 0) {
      if (self->sectionDepth == 1) {
        // Finalize the previous section's length
        size_t endOffset = XML_GetCurrentByteIndex(static_cast<XML_Parser>(self->parser));
        // Estimate end position (expat gives us the start of the end tag)
        // Add a reasonable estimate for the closing tag length
        endOffset += strlen(name) + 3;  // "</section>" roughly

        Fb2::SectionInfo info;
        info.title = self->currentSectionTitle;
        info.fileOffset = self->currentSectionOffset;
        info.length = endOffset - self->currentSectionOffset;
        self->sections.push_back(std::move(info));

        // Add TOC entry
        Fb2::TocEntry tocEntry;
        tocEntry.title = self->sections.back().title.empty()
                             ? ("Section " + std::to_string(self->currentSectionIndex + 1))
                             : self->sections.back().title;
        tocEntry.sectionIndex = self->currentSectionIndex;
        self->tocEntries.push_back(std::move(tocEntry));

        self->currentSectionIndex++;
        self->previousSectionEnd = endOffset;
      }
      self->sectionDepth--;
    } else if (strcmp(tag, "body") == 0) {
      self->inBody = false;
    }
  }
}

void Fb2MetadataParser::characterData(void* userData, const char* s, int len) {
  auto* self = static_cast<Fb2MetadataParser*>(userData);

  if (self->context == Context::BOOK_TITLE || self->context == Context::AUTHOR_FIRST_NAME ||
      self->context == Context::AUTHOR_MIDDLE_NAME || self->context == Context::AUTHOR_LAST_NAME ||
      self->context == Context::LANG || self->context == Context::SECTION_TITLE_P) {
    self->charBuffer.append(s, len);
  }
}

bool Fb2MetadataParser::parse() {
  XML_Parser xmlParser = XML_ParserCreate(nullptr);
  if (!xmlParser) {
    Serial.printf("[%lu] [FB2] Could not create XML parser\n", millis());
    return false;
  }

  parser = xmlParser;

  XML_SetUserData(xmlParser, this);
  XML_SetElementHandler(xmlParser, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser, characterData);

  FsFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    XML_ParserFree(xmlParser);
    parser = nullptr;
    return false;
  }

  bool success = true;
  int done;
  do {
    void* const buf = XML_GetBuffer(xmlParser, 1024);
    if (!buf) {
      Serial.printf("[%lu] [FB2] Could not allocate parser buffer\n", millis());
      success = false;
      break;
    }

    const size_t len = file.read(buf, 1024);
    if (len == 0 && file.available() > 0) {
      Serial.printf("[%lu] [FB2] File read error\n", millis());
      success = false;
      break;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(xmlParser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      Serial.printf("[%lu] [FB2] Parse error at line %lu: %s\n", millis(), XML_GetCurrentLineNumber(xmlParser),
                    XML_ErrorString(XML_GetErrorCode(xmlParser)));
      success = false;
      break;
    }
  } while (!done);

  XML_ParserFree(xmlParser);
  parser = nullptr;
  file.close();

  // If no sections found, treat entire body as one section
  if (success && sections.empty()) {
    Serial.printf("[%lu] [FB2] No sections found, treating entire file as one section\n", millis());
    // Re-parse is too expensive; create a dummy section covering the whole file
    FsFile sizeFile;
    if (Storage.openFileForRead("FB2", filepath, sizeFile)) {
      Fb2::SectionInfo info;
      info.title = title.empty() ? "Content" : title;
      info.fileOffset = 0;
      info.length = sizeFile.size();
      sections.push_back(std::move(info));
      sizeFile.close();

      Fb2::TocEntry tocEntry;
      tocEntry.title = sections.back().title;
      tocEntry.sectionIndex = 0;
      tocEntries.push_back(std::move(tocEntry));
    }
  }

  return success;
}
