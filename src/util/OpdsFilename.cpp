#include "OpdsFilename.h"

#include "StringUtils.h"

std::string opdsBookFilename(const std::string& author, const std::string& title, const OpdsFilenameFormat format,
                             const std::string_view extension) {
  std::string base;
  switch (format) {
    case OpdsFilenameFormat::TitleAuthor:
      base = author.empty() ? title : title + " - " + author;
      break;
    case OpdsFilenameFormat::TitleOnly:
      base = title;
      break;
    case OpdsFilenameFormat::AuthorTitle:
    default:
      base = author.empty() ? title : author + " - " + title;
      break;
  }
  // Keep the trusted format extension outside the 100-byte sanitized base.
  std::string filename = StringUtils::sanitizeFilename(base);
  filename.append(extension.data(), extension.size());
  return filename;
}
