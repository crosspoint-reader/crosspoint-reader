#include "ClippingStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
std::string escapeYamlString(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    if (c == '"')
      result += "\\\"";
    else if (c == '\\')
      result += "\\\\";
    else
      result += c;
  }
  return result;
}
}  // namespace

std::string ClippingStore::getBasePath(const std::string& bookPath) {
  // FNV-1a hash of full book path (same algorithm as BookmarkStore)
  uint32_t hash = 2166136261u;
  for (const char c : bookPath) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  char hexHash[9];
  snprintf(hexHash, sizeof(hexHash), "%08x", hash);
  return std::string(CLIPPINGS_DIR) + "/" + hexHash;
}

std::string ClippingStore::getIndexPath(const std::string& bookPath) { return getBasePath(bookPath) + ".idx"; }

std::string ClippingStore::getMdPath(const std::string& bookPath) { return getBasePath(bookPath) + ".md"; }

bool ClippingStore::writeIndex(const std::string& path, const std::vector<ClippingEntry>& entries) {
  FsFile file;
  if (!Storage.openFileForWrite(TAG, path, file)) {
    return false;
  }

  // Write magic "CIDX"
  if (file.write(reinterpret_cast<const uint8_t*>(INDEX_MAGIC), 4) != 4) {
    file.close();
    return false;
  }

  // Write version byte
  uint8_t version = FORMAT_VERSION;
  if (file.write(&version, 1) != 1) {
    file.close();
    return false;
  }

  // Write count as 2-byte LE
  if (entries.size() > UINT16_MAX) {
    LOG_ERR(TAG, "Too many clipping entries (%d)", static_cast<int>(entries.size()));
    file.close();
    return false;
  }
  uint16_t count = static_cast<uint16_t>(entries.size());
  uint8_t countBytes[2] = {static_cast<uint8_t>(count & 0xFF), static_cast<uint8_t>((count >> 8) & 0xFF)};
  if (file.write(countBytes, 2) != 2) {
    file.close();
    return false;
  }

  // Write each entry as 16 bytes LE
  for (const auto& entry : entries) {
    uint8_t data[16];
    data[0] = entry.textOffset & 0xFF;
    data[1] = (entry.textOffset >> 8) & 0xFF;
    data[2] = (entry.textOffset >> 16) & 0xFF;
    data[3] = (entry.textOffset >> 24) & 0xFF;
    data[4] = entry.textLength & 0xFF;
    data[5] = (entry.textLength >> 8) & 0xFF;
    data[6] = (entry.textLength >> 16) & 0xFF;
    data[7] = (entry.textLength >> 24) & 0xFF;
    data[8] = entry.bookPercent;
    data[9] = entry.chapterPercent;
    data[10] = entry.spineIndex & 0xFF;
    data[11] = (entry.spineIndex >> 8) & 0xFF;
    data[12] = entry.startPage & 0xFF;
    data[13] = (entry.startPage >> 8) & 0xFF;
    data[14] = entry.endPage & 0xFF;
    data[15] = (entry.endPage >> 8) & 0xFF;
    if (file.write(data, 16) != 16) {
      file.close();
      return false;
    }
  }

  file.close();
  return true;
}

std::vector<ClippingEntry> ClippingStore::loadIndex(const std::string& bookPath) {
  std::vector<ClippingEntry> entries;
  const std::string path = getIndexPath(bookPath);

  FsFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    return entries;
  }

  // Read and validate magic
  char magic[4];
  if (file.read(magic, 4) != 4 || memcmp(magic, INDEX_MAGIC, 4) != 0) {
    LOG_ERR(TAG, "Invalid index magic in %s", path.c_str());
    file.close();
    return entries;
  }

  // Read and validate version
  uint8_t version;
  if (file.read(&version, 1) != 1 || version != FORMAT_VERSION) {
    LOG_DBG(TAG, "Skipping index with version %d (expected %d): %s", version, FORMAT_VERSION, path.c_str());
    file.close();
    return entries;
  }

  // Read count (2-byte LE)
  uint8_t countBytes[2];
  if (file.read(countBytes, 2) != 2) {
    file.close();
    return entries;
  }
  uint16_t count = countBytes[0] | (countBytes[1] << 8);

  // Read entries
  for (uint16_t i = 0; i < count; i++) {
    uint8_t data[16];
    if (file.read(data, 16) != 16) {
      break;
    }
    ClippingEntry entry;
    entry.textOffset = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                       (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    entry.textLength = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
                       (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
    entry.bookPercent = data[8];
    entry.chapterPercent = data[9];
    entry.spineIndex = data[10] | (data[11] << 8);
    entry.startPage = data[12] | (data[13] << 8);
    entry.endPage = data[14] | (data[15] << 8);
    entries.push_back(entry);
  }

  file.close();
  return entries;
}

bool ClippingStore::saveClipping(const std::string& bookPath, const std::string& bookTitle,
                                 const std::string& bookAuthor, const std::vector<CapturedPage>& pages) {
  if (pages.empty()) {
    return false;
  }

  Storage.mkdir(CLIPPINGS_DIR);

  const std::string mdPath = getMdPath(bookPath);
  const std::string idxPath = getIndexPath(bookPath);

  // Build the markdown text block (same format as PageExporter::writePassage)
  std::string textBlock;
  std::string lastChapter;
  for (const auto& page : pages) {
    if (page.chapterTitle != lastChapter) {
      textBlock += "\n## " + page.chapterTitle + " | " + std::to_string(page.bookPercent) + "% of book | " +
                   std::to_string(page.chapterPercent) + "% of chapter\n";
      lastChapter = page.chapterTitle;
    }
    textBlock += page.pageText;
    textBlock += "\n\n";
  }
  textBlock += "---\n";

  // Check if .md file is new (need to write header)
  const bool isNew = !Storage.exists(mdPath.c_str());

  // Open .md in append mode
  FsFile mdFile = Storage.open(mdPath.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!mdFile) {
    LOG_ERR(TAG, "Failed to open md file: %s", mdPath.c_str());
    return false;
  }

  // Write header for new files
  if (isNew) {
    std::string header = "---\n";
    header += "title: \"" + escapeYamlString(bookTitle) + "\"\n";
    if (!bookAuthor.empty()) {
      header += "author: \"" + escapeYamlString(bookAuthor) + "\"\n";
    }
    header += "---\n\n";
    header += "# " + bookTitle;
    if (!bookAuthor.empty()) {
      header += " \xe2\x80\x94 " + bookAuthor;  // em-dash UTF-8
    }
    header += "\n";
    if (mdFile.write(reinterpret_cast<const uint8_t*>(header.c_str()), header.size()) != header.size()) {
      LOG_ERR(TAG, "Failed to write header");
      mdFile.close();
      return false;
    }
  }

  // Record offset before writing text block
  uint32_t textOffset = mdFile.size();

  // Write the text block
  uint32_t textLength = textBlock.size();
  if (mdFile.write(reinterpret_cast<const uint8_t*>(textBlock.c_str()), textLength) != textLength) {
    LOG_ERR(TAG, "Failed to write text block");
    mdFile.close();
    return false;
  }

  mdFile.close();

  // Build the ClippingEntry
  ClippingEntry entry;
  entry.textOffset = textOffset;
  entry.textLength = textLength;
  entry.bookPercent = static_cast<uint8_t>(pages[0].bookPercent);
  entry.chapterPercent = static_cast<uint8_t>(pages[0].chapterPercent);
  entry.spineIndex = pages[0].spineIndex;
  entry.startPage = pages[0].pageIndex;
  entry.endPage = pages.back().pageIndex;

  // Load existing index, append new entry, write index
  auto entries = loadIndex(bookPath);
  entries.push_back(entry);

  const bool ok = writeIndex(idxPath, entries);
  if (ok) {
    LOG_DBG(TAG, "Clipping saved at %d%% (total: %d)", entry.bookPercent, static_cast<int>(entries.size()));
  }
  return ok;
}

std::string ClippingStore::loadClippingText(const std::string& bookPath, const ClippingEntry& entry) {
  const std::string mdPath = getMdPath(bookPath);

  FsFile file;
  if (!Storage.openFileForRead(TAG, mdPath, file)) {
    return "";
  }

  if (!file.seekSet(entry.textOffset)) {
    file.close();
    return "";
  }

  std::string text;
  text.resize(entry.textLength);
  int bytesRead = file.read(reinterpret_cast<uint8_t*>(&text[0]), entry.textLength);
  file.close();

  if (bytesRead != static_cast<int>(entry.textLength)) {
    return "";
  }
  return text;
}

std::string ClippingStore::loadClippingPreview(const std::string& bookPath, const ClippingEntry& entry, int maxChars) {
  const std::string mdPath = getMdPath(bookPath);

  FsFile file;
  if (!Storage.openFileForRead(TAG, mdPath, file)) {
    return "";
  }

  if (!file.seekSet(entry.textOffset)) {
    file.close();
    return "";
  }

  uint32_t readLen = std::min(static_cast<uint32_t>(maxChars), entry.textLength);
  std::string text;
  text.resize(readLen);
  int bytesRead = file.read(reinterpret_cast<uint8_t*>(&text[0]), readLen);
  file.close();

  if (bytesRead <= 0) {
    return "";
  }
  text.resize(bytesRead);

  // Strip leading whitespace/newlines and markdown headings (## ...)
  size_t start = 0;
  while (start < text.size()) {
    // Skip whitespace
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\n' || text[start] == '\r' || text[start] == '\t')) {
      start++;
    }
    // Skip markdown headings and separators
    if (start < text.size() && text[start] == '#') {
      size_t lineEnd = text.find('\n', start);
      start = (lineEnd == std::string::npos) ? text.size() : lineEnd + 1;
      continue;
    }
    if (start + 2 < text.size() && text[start] == '-' && text[start + 1] == '-' && text[start + 2] == '-') {
      size_t lineEnd = text.find('\n', start);
      start = (lineEnd == std::string::npos) ? text.size() : lineEnd + 1;
      continue;
    }
    break;
  }
  text = text.substr(start);

  // Replace newlines with spaces for single-line preview
  for (auto& ch : text) {
    // cppcheck-suppress useStlAlgorithm
    if (ch == '\n' || ch == '\r') ch = ' ';
  }

  // Append ellipsis if truncated
  if (readLen < entry.textLength || start > 0) {
    text += "...";
  }

  return text;
}

bool ClippingStore::hasClippingAtPage(const std::vector<ClippingEntry>& entries, uint16_t spineIndex,
                                      uint16_t pageIndex) {
  return std::any_of(entries.begin(), entries.end(), [spineIndex, pageIndex](const ClippingEntry& e) {
    return e.spineIndex == spineIndex && pageIndex >= e.startPage && pageIndex <= e.endPage;
  });
}

bool ClippingStore::deleteClipping(const std::string& bookPath, int index) {
  auto entries = loadIndex(bookPath);

  if (index < 0 || index >= static_cast<int>(entries.size())) {
    return false;
  }

  const std::string mdPath = getMdPath(bookPath);
  const std::string idxPath = getIndexPath(bookPath);

  // Read all clipping texts except the deleted one
  std::vector<std::string> texts;
  for (int i = 0; i < static_cast<int>(entries.size()); i++) {
    if (i == index) continue;
    std::string text = loadClippingText(bookPath, entries[i]);
    if (text.empty()) {
      LOG_ERR(TAG, "Failed to read clipping %d during delete", i);
      return false;
    }
    texts.push_back(text);
  }

  // Remove the entry
  entries.erase(entries.begin() + index);

  // Read the file header (YAML frontmatter + title) before the first clipping
  std::string header;
  if (!entries.empty()) {
    FsFile origFile;
    if (Storage.openFileForRead(TAG, mdPath, origFile)) {
      // Find the minimum textOffset among remaining entries to determine header size
      uint32_t minOffset = origFile.size();
      auto it = std::min_element(entries.begin(), entries.end(), [](const ClippingEntry& a, const ClippingEntry& b) {
        return a.textOffset < b.textOffset;
      });
      if (it != entries.end()) {
        minOffset = it->textOffset;
      }
      if (minOffset > 0) {
        header.resize(minOffset);
        origFile.seekSet(0);
        origFile.read(reinterpret_cast<uint8_t*>(&header[0]), minOffset);
      }
      origFile.close();
    }
  }

  // Rewrite the .md file
  FsFile mdFile;
  if (!Storage.openFileForWrite(TAG, mdPath, mdFile)) {
    return false;
  }

  // Write header
  if (!header.empty()) {
    if (mdFile.write(reinterpret_cast<const uint8_t*>(header.c_str()), header.size()) != header.size()) {
      LOG_ERR(TAG, "Failed to write header during delete");
      mdFile.close();
      return false;
    }
  }

  // Write each remaining clipping and update offsets
  for (size_t i = 0; i < texts.size(); i++) {
    entries[i].textOffset = mdFile.size();
    entries[i].textLength = texts[i].size();
    if (mdFile.write(reinterpret_cast<const uint8_t*>(texts[i].c_str()), texts[i].size()) != texts[i].size()) {
      LOG_ERR(TAG, "Failed to write clipping %d during delete", static_cast<int>(i));
      mdFile.close();
      return false;
    }
  }

  mdFile.close();

  // Rewrite the index
  const bool ok = writeIndex(idxPath, entries);
  if (ok) {
    LOG_DBG(TAG, "Clipping deleted (remaining: %d)", static_cast<int>(entries.size()));
  }
  return ok;
}
