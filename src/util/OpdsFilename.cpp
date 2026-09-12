#include "OpdsFilename.h"

#include <cctype>

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
  // Full whitespace predicate, not just space/tab: the header promises to
  // trim "surrounding whitespace", and a web API caller can send a value with
  // an embedded newline or CR that a single-line on-device keyboard field never would.
  auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!path.empty() && isSpace(path.front())) path.erase(path.begin());
  while (!path.empty() && isSpace(path.back())) path.pop_back();
  if (path.empty()) return "";
  if (path.front() != '/') path.insert(path.begin(), '/');
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path == "/") return "";  // a bare slash is SD root, same as empty
  return path;
}
