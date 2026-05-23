#include "PageMapParser.h"

#include <FsHelpers.h>
#include <Logging.h>

bool PageMapParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("PMP", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, nullptr);
  return true;
}

PageMapParser::~PageMapParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t PageMapParser::write(const uint8_t data) { return write(&data, 1); }

size_t PageMapParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("PMP", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("PMP", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
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

void XMLCALL PageMapParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<PageMapParser*>(userData);

  // We only care about <page name="..." href="..."/> elements. The wrapping <page-map>
  // root is ignored (no need for a state machine — every page element carries its data).
  const char* localName = strrchr(name, ':');
  if (localName) {
    localName++;
  } else {
    localName = name;
  }
  if (strcmp(localName, "page") != 0) {
    return;
  }

  std::string label;
  std::string rawHref;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "name") == 0) {
      label = atts[i + 1];
    } else if (strcmp(atts[i], "href") == 0) {
      rawHref = atts[i + 1];
    }
  }

  if (label.empty() || rawHref.empty()) {
    return;
  }

  std::string href = FsHelpers::normalisePath(self->baseContentPath + rawHref);
  std::string anchor;
  const size_t pos = href.find('#');
  if (pos != std::string::npos) {
    anchor = href.substr(pos + 1);
    href = href.substr(0, pos);
  }
  self->pageList.push_back({std::move(href), std::move(anchor), std::move(label)});
}
