#include "TocNavParser.h"

#include <FsHelpers.h>
#include <Logging.h>

#include "../BookMetadataCache.h"

bool TocNavParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("NAV", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

TocNavParser::~TocNavParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t TocNavParser::write(const uint8_t data) { return write(&data, 1); }

size_t TocNavParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("NAV", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("NAV", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
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

void XMLCALL TocNavParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<TocNavParser*>(userData);

  // Track HTML structure loosely - we mainly care about finding <nav epub:type="toc">
  if (strcmp(name, "html") == 0) {
    self->state = IN_HTML;
    return;
  }

  if (self->state == IN_HTML && strcmp(name, "body") == 0) {
    self->state = IN_BODY;
    return;
  }

  // Look for <nav epub:type="toc"> or <nav epub:type="page-list"> anywhere in body.
  // Both navs are siblings under <body>; we don't expect them to nest.
  if (self->state >= IN_BODY && self->state != IN_NAV_TOC && self->state != IN_NAV_PAGE_LIST &&
      strcmp(name, "nav") == 0) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "epub:type") == 0 || strcmp(atts[i], "type") == 0) {
        if (strcmp(atts[i + 1], "toc") == 0) {
          self->state = IN_NAV_TOC;
          LOG_DBG("NAV", "Found nav toc element");
          return;
        }
        if (strcmp(atts[i + 1], "page-list") == 0) {
          self->state = IN_NAV_PAGE_LIST;
          LOG_DBG("NAV", "Found nav page-list element");
          return;
        }
      }
    }
    return;
  }

  // Page-list nav: parallel state machine (independent ol/li/a tracking).
  if (self->state >= IN_NAV_PAGE_LIST && self->state <= IN_PL_ANCHOR) {
    if (strcmp(name, "ol") == 0) {
      self->plOlDepth++;
      self->state = IN_PL_OL;
      return;
    }
    if (self->state == IN_PL_OL && strcmp(name, "li") == 0) {
      self->state = IN_PL_LI;
      self->currentPageLabel.clear();
      self->currentPageHref.clear();
      return;
    }
    if (self->state == IN_PL_LI && strcmp(name, "a") == 0) {
      self->state = IN_PL_ANCHOR;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "href") == 0) {
          self->currentPageHref = atts[i + 1];
          break;
        }
      }
      return;
    }
    return;
  }

  // Only process ol/li/a if we're inside the toc nav
  if (self->state < IN_NAV_TOC) {
    return;
  }

  if (strcmp(name, "ol") == 0) {
    self->olDepth++;
    self->state = IN_OL;
    return;
  }

  if (self->state == IN_OL && strcmp(name, "li") == 0) {
    self->state = IN_LI;
    self->currentLabel.clear();
    self->currentHref.clear();
    return;
  }

  if (self->state == IN_LI && strcmp(name, "a") == 0) {
    self->state = IN_ANCHOR;
    // Get href attribute
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "href") == 0) {
        self->currentHref = atts[i + 1];
        break;
      }
    }
    return;
  }
}

void XMLCALL TocNavParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<TocNavParser*>(userData);

  // Collect text inside the anchor of either nav (TOC or page-list).
  if (self->state == IN_ANCHOR) {
    self->currentLabel.append(s, len);
  } else if (self->state == IN_PL_ANCHOR) {
    self->currentPageLabel.append(s, len);
  }
}

void XMLCALL TocNavParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<TocNavParser*>(userData);

  // ---- Page-list nav close handlers (checked before TOC handlers because IN_PL_* states
  // sort after IN_NAV_TOC, but we want exact-state matching either way).
  if (strcmp(name, "a") == 0 && self->state == IN_PL_ANCHOR) {
    if (!self->currentPageLabel.empty() && !self->currentPageHref.empty()) {
      std::string href = FsHelpers::normalisePath(self->baseContentPath + self->currentPageHref);
      std::string anchor;
      const size_t pos = href.find('#');
      if (pos != std::string::npos) {
        anchor = href.substr(pos + 1);
        href = href.substr(0, pos);
      }
      self->pageList.push_back({std::move(href), std::move(anchor), std::move(self->currentPageLabel)});
      self->currentPageLabel.clear();
      self->currentPageHref.clear();
    }
    self->state = IN_PL_LI;
    return;
  }

  if (strcmp(name, "li") == 0 && (self->state == IN_PL_LI || self->state == IN_PL_OL)) {
    self->state = IN_PL_OL;
    return;
  }

  if (strcmp(name, "ol") == 0 &&
      (self->state == IN_PL_OL || self->state == IN_PL_LI || self->state == IN_NAV_PAGE_LIST)) {
    if (self->plOlDepth > 0) {
      self->plOlDepth--;
    }
    self->state = (self->plOlDepth == 0) ? IN_NAV_PAGE_LIST : IN_PL_LI;
    return;
  }

  if (strcmp(name, "nav") == 0 &&
      (self->state == IN_NAV_PAGE_LIST || self->state == IN_PL_OL || self->state == IN_PL_LI)) {
    self->state = IN_BODY;
    self->plOlDepth = 0;
    LOG_DBG("NAV", "Finished parsing nav page-list");
    return;
  }

  // ---- TOC nav close handlers
  if (strcmp(name, "a") == 0 && self->state == IN_ANCHOR) {
    // Create TOC entry when closing anchor tag (we have all data now)
    if (!self->currentLabel.empty() && !self->currentHref.empty()) {
      std::string href = FsHelpers::normalisePath(self->baseContentPath + self->currentHref);
      std::string anchor;

      const size_t pos = href.find('#');
      if (pos != std::string::npos) {
        anchor = href.substr(pos + 1);
        href = href.substr(0, pos);
      }

      if (self->cache) {
        // olDepth gives us the nesting level (1-based from the outer ol)
        self->cache->createTocEntry(self->currentLabel, href, anchor, self->olDepth);
      }

      self->currentLabel.clear();
      self->currentHref.clear();
    }
    self->state = IN_LI;
    return;
  }

  if (strcmp(name, "li") == 0 && (self->state == IN_LI || self->state == IN_OL)) {
    self->state = IN_OL;
    return;
  }

  if (strcmp(name, "ol") == 0 && (self->state == IN_OL || self->state == IN_LI || self->state == IN_NAV_TOC)) {
    if (self->olDepth > 0) {
      self->olDepth--;
    }
    self->state = (self->olDepth == 0) ? IN_NAV_TOC : IN_LI;
    return;
  }

  if (strcmp(name, "nav") == 0 && (self->state == IN_NAV_TOC || self->state == IN_OL || self->state == IN_LI)) {
    self->state = IN_BODY;
    self->olDepth = 0;
    LOG_DBG("NAV", "Finished parsing nav toc");
    return;
  }
}
