#include "PageExporter.h"

#include <SDCardManager.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr const char* EXPORTS_DIR = "/.crosspoint/exports";
constexpr const char* TAG = "PEX";
}  // namespace

std::string PageExporter::sanitizeFilename(const std::string& title) {
  std::string result;
  result.reserve(title.size());
  for (char c : title) {
    if (c == ' ') {
      result += '_';
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      result += c;
    }
    // Skip all other characters (punctuation, special chars, etc.)
  }
  // Truncate to 80 chars to stay well within FAT32 limits
  if (result.size() > 80) {
    result.resize(80);
  }
  if (result.empty()) {
    result = "untitled";
  }
  return result;
}

std::string PageExporter::getExportPath(const std::string& bookTitle, const std::string& bookHash) {
  std::string filename;
  if (bookTitle.empty()) {
    filename = bookHash;
  } else {
    filename = sanitizeFilename(bookTitle);
  }
  return std::string(EXPORTS_DIR) + "/" + filename + ".txt";
}

bool PageExporter::writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor) {
  std::string header = "== " + bookTitle;
  if (!bookAuthor.empty()) {
    header += " \xe2\x80\x94 " + bookAuthor;  // em-dash UTF-8
  }
  header += " ==\n";
  return file.write(reinterpret_cast<const uint8_t*>(header.c_str()), header.size()) == header.size();
}

bool PageExporter::writeEntry(FsFile& file, const std::string& chapterTitle, int pageNumber, int bookPercent,
                              const std::string& pageText) {
  char meta[128];
  snprintf(meta, sizeof(meta), "\n--- %s | Page %d | %d%% ---\n", chapterTitle.c_str(), pageNumber, bookPercent);
  std::string entry(meta);
  entry += pageText;
  entry += '\n';
  return file.write(reinterpret_cast<const uint8_t*>(entry.c_str()), entry.size()) == entry.size();
}

bool PageExporter::exportPage(const std::string& bookTitle, const std::string& bookAuthor,
                              const std::string& bookHash, const std::string& chapterTitle,
                              int pageNumber, int bookPercent, const std::string& pageText) {
  SdMan.mkdir(EXPORTS_DIR);

  const std::string path = getExportPath(bookTitle, bookHash);

  // Check if file already exists (to decide whether to write header)
  const bool isNew = !SdMan.exists(path.c_str());

  // Open in append mode
  FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open export file: %s\n", millis(), TAG, path.c_str());
    return false;
  }

  bool ok = true;
  if (isNew) {
    ok = writeHeader(file, bookTitle, bookAuthor);
  }
  if (ok) {
    ok = writeEntry(file, chapterTitle, pageNumber, bookPercent, pageText);
  }

  file.close();

  if (ok) {
    Serial.printf("[%lu] [%s] Page exported to %s\n", millis(), TAG, path.c_str());
  } else {
    Serial.printf("[%lu] [%s] Failed to write export entry\n", millis(), TAG);
  }
  return ok;
}
