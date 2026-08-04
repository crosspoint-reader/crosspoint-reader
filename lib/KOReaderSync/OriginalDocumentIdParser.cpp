#include "OriginalDocumentIdParser.h"

#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>
#include <utility>

namespace {
constexpr char ORIGINAL_DOCUMENT_ID_META_NAME[] = "crosspoint:original-koreader-document-id";

bool normalizeDocumentId(const char* value, std::string& normalized) {
  constexpr size_t MD5_HEX_LENGTH = 32;
  if (!value || strlen(value) != MD5_HEX_LENGTH) {
    return false;
  }

  normalized.clear();
  normalized.reserve(MD5_HEX_LENGTH);
  for (size_t i = 0; i < MD5_HEX_LENGTH; i++) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (!std::isxdigit(c)) {
      normalized.clear();
      return false;
    }
    normalized.push_back(static_cast<char>(std::tolower(c)));
  }
  return true;
}
}  // namespace

bool OriginalDocumentIdParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  return true;
}

OriginalDocumentIdParser::~OriginalDocumentIdParser() { destroyXmlParser(parser); }

size_t OriginalDocumentIdParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || size > remainingSize) {
    return 0;
  }

  if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), remainingSize == size) ==
      XML_STATUS_ERROR) {
    destroyXmlParser(parser);
    return 0;
  }

  remainingSize -= size;
  return size;
}

void XMLCALL OriginalDocumentIdParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OriginalDocumentIdParser*>(userData);

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state != IN_METADATA || (strcmp(name, "meta") != 0 && strcmp(name, "opf:meta") != 0)) {
    return;
  }
  if (!self->documentId.empty()) {
    return;
  }

  const char* metaName = nullptr;
  const char* content = nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "name") == 0) {
      metaName = atts[i + 1];
    } else if (strcmp(atts[i], "content") == 0) {
      content = atts[i + 1];
    }
  }

  if (metaName && strcmp(metaName, ORIGINAL_DOCUMENT_ID_META_NAME) == 0) {
    std::string normalized;
    if (normalizeDocumentId(content, normalized)) {
      self->documentId = std::move(normalized);
    }
  }
}

void XMLCALL OriginalDocumentIdParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OriginalDocumentIdParser*>(userData);

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
  } else if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
  }
}
