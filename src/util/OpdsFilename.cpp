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

std::string normalizeOpdsFolder(std::string path) {
  while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(path.begin());
  while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();
  if (path.empty()) return "";
  if (path.front() != '/') path.insert(path.begin(), '/');
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path == "/") return "";  // a bare slash is SD root, same as empty
  return path;
}
