#include "BookmarkUtil.h"

#include <Utf8.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getBookmarksDir() + bookName;
}

std::string BookmarkUtil::sanitizeBookmarkSummary(std::string summary) {
  summary.erase(
      std::unique(summary.begin(), summary.end(), [](char a, char b) { return std::isspace(a) && std::isspace(b); }),
      summary.end());
  summary.erase(std::remove(summary.begin(), summary.end(), '\n'), summary.end());
  summary.erase(summary.begin(),
                std::find_if(summary.begin(), summary.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  summary.erase(
      std::find_if(summary.rbegin(), summary.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
      summary.end());
  // Cut on a character boundary; this also drops a split character left by older firmware.
  summary.resize(utf8SafeTruncateBuffer(summary.data(), std::min<int>(summary.size(), 72)));
  return summary;
}

uint64_t BookmarkUtil::syncId(const std::string& xpath) {
  uint8_t digest[32] = {};
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(xpath.data()), xpath.size(), digest, 0) != 0) return 0;
  uint64_t id = 0;
  for (size_t i = 0; i < 8; ++i) id = id << 8 | digest[i];
  return id;
}

void BookmarkUtil::formatSyncId(const uint64_t id, char* out) {
  snprintf(out, SYNC_ID_HEX_SIZE, "%016llx", static_cast<unsigned long long>(id));
}

bool BookmarkUtil::parseSyncId(const char* hex, uint64_t& id) {
  if (!hex || strlen(hex) != SYNC_ID_HEX_SIZE - 1 || strspn(hex, "0123456789abcdef") != SYNC_ID_HEX_SIZE - 1) {
    return false;
  }
  id = strtoull(hex, nullptr, 16);
  return true;
}
