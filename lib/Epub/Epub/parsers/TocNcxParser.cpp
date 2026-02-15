#include "TocNcxParser.h"

#include <FsHelpers.h>
#include <Logging.h>

#include "../BookMetadataCache.h"

bool TocNcxParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("TOC", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

TocNcxParser::~TocNcxParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
    XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t TocNcxParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNcxParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("TOC", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("TOC", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }
  return size;
}

void XMLCALL TocNcxParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  // NOTE: We rely on navPoint label and content coming before any nested navPoints, this will be fine:
  // <navPoint>
  //   <navLabel><text>Chapter 1</text></navLabel>
  //   <content src="ch1.html"/>
  //   <navPoint> ...nested... </navPoint>
  // </navPoint>
  //
  // This will NOT:
  // <navPoint>
  //   <navPoint> ...nested... </navPoint>
  //   <navLabel><text>Chapter 1</text></navLabel>
  //   <content src="ch1.html"/>
  // </navPoint>

  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == START && strcmp(name, "ncx") == 0) {
    self->state = IN_NCX;
    return;
  }

  if (self->state == IN_NCX && strcmp(name, "navMap") == 0) {
    self->state = IN_NAV_MAP;
    return;
  }

  // Handles both top-level and nested navPoints
  if ((self->state == IN_NAV_MAP || self->state == IN_NAV_POINT) && strcmp(name, "navPoint") == 0) {
    self->state = IN_NAV_POINT;
    self->currentDepth++;

    self->currentLabel.clear();
    self->currentSrc.clear();
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL_TEXT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "src") == 0) {
        self->currentSrc = atts[i + 1];
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNcxParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNcxParser*>(userData);
  if (self->state == IN_NAV_LABEL_TEXT) {
    self->currentLabel.append(s, len);
  }
}

void XMLCALL TocNcxParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNcxParser*>(userData);

  if (self->state == IN_NAV_LABEL_TEXT && strcmp(name, "text") == 0) {
    self->state = IN_NAV_LABEL;
    return;
  }

  if (self->state == IN_NAV_LABEL && strcmp(name, "navLabel") == 0) {
    self->state = IN_NAV_POINT;
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "navPoint") == 0) {
    self->currentDepth--;
    if (self->currentDepth == 0) {
      self->state = IN_NAV_MAP;
    }
    return;
  }

  if (self->state == IN_NAV_POINT && strcmp(name, "content") == 0) {
    // At this point (end of content tag), we likely have both Label (from previous tags) and Src.
    // This is the safest place to push the data, assuming <navLabel> always comes before <content>.
    // NCX spec says navLabel comes before content.
    if (!self->currentLabel.empty() && !self->currentSrc.empty()) {
      std::string href = FsHelpers::normalisePath(self->baseContentPath + self->currentSrc);
      std::string anchor;

      const size_t pos = href.find('#');
      if (pos != std::string::npos) {
        anchor = href.substr(pos + 1);
        href = href.substr(0, pos);
      }

      if (self->cache) {
        self->cache->createTocEntry(self->currentLabel, href, anchor, self->currentDepth);
      }

      // Clear them so we don't re-add them if there are weird XML structures
      self->currentLabel.clear();
      self->currentSrc.clear();
    }
  }
}
