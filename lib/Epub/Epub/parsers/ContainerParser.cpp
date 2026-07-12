#include "ContainerParser.h"

#include <Logging.h>

bool ContainerParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("CTR", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  return true;
}

ContainerParser::~ContainerParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
    XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t ContainerParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContainerParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_DBG("CTR", "Couldn't allocate buffer");
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_ERR("CTR", "Parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }
  return size;
}

void XMLCALL ContainerParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContainerParser*>(userData);

  // Simple state tracking to ensure we are looking at the valid schema structure
  if (self->state == START && strcmp(name, "container") == 0) {
    self->state = IN_CONTAINER;
    return;
  }

  if (self->state == IN_CONTAINER && strcmp(name, "rootfiles") == 0) {
    self->state = IN_ROOTFILES;
    return;
  }

  if (self->state == IN_ROOTFILES && strcmp(name, "rootfile") == 0) {
    const char* mediaType = nullptr;
    const char* path = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "full-path") == 0) {
        path = atts[i + 1];
      }
    }

    // Check if this is the standard OEBPS package
    if (mediaType && path && strcmp(mediaType, "application/oebps-package+xml") == 0) {
      self->fullPath = path;
    }
  }
}

void XMLCALL ContainerParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContainerParser*>(userData);

  if (self->state == IN_ROOTFILES && strcmp(name, "rootfiles") == 0) {
    self->state = IN_CONTAINER;
  } else if (self->state == IN_CONTAINER && strcmp(name, "container") == 0) {
    self->state = START;
  }
}
