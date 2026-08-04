#include "OriginalDocumentIdMetadata.h"

#include <cctype>
#include <cstring>
#include <utility>

namespace {
constexpr char ORIGINAL_DOCUMENT_ID_META_NAME[] = "crosspoint:original-koreader-document-id";
constexpr size_t MD5_HEX_LENGTH = 32;
}  // namespace

bool extractOriginalDocumentIdMetadata(const char* const* attributes, std::string& documentId) {
  const char* metaName = nullptr;
  const char* content = nullptr;
  for (int i = 0; attributes[i]; i += 2) {
    if (strcmp(attributes[i], "name") == 0) {
      metaName = attributes[i + 1];
    } else if (strcmp(attributes[i], "content") == 0) {
      content = attributes[i + 1];
    }
  }

  if (!metaName || strcmp(metaName, ORIGINAL_DOCUMENT_ID_META_NAME) != 0 || !content ||
      strlen(content) != MD5_HEX_LENGTH) {
    return false;
  }

  std::string normalized;
  normalized.reserve(MD5_HEX_LENGTH);
  for (size_t i = 0; i < MD5_HEX_LENGTH; i++) {
    const auto c = static_cast<unsigned char>(content[i]);
    if (!std::isxdigit(c)) {
      return false;
    }
    normalized.push_back(static_cast<char>(std::tolower(c)));
  }

  documentId = std::move(normalized);
  return true;
}
