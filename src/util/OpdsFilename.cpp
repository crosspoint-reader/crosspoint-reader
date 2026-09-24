#include "OpdsFilename.h"

#include "StringUtils.h"

std::string opdsBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format) {
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
  // sanitizeFilename caps at 100 bytes and never returns empty (falls back to
  // "book"); ".epub" is appended after so the extension is never truncated —
  // identical treatment to the previous inline construction.
  return StringUtils::sanitizeFilename(base) + ".epub";
}
