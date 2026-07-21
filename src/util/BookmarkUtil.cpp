#include "BookmarkUtil.h"

#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

std::string BookmarkUtil::getBookmarksDir() { return "/.crosspoint/bookmarks/"; }

std::string BookmarkUtil::getBookmarkPath(const std::string& bookPath) {
  constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  uint64_t hash = FNV_OFFSET;
  for (const unsigned char byte : bookPath) {
    hash ^= byte;
    hash *= FNV_PRIME;
  }
  constexpr std::array<char, 16> HEX_DIGITS = {'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::array<char, 16> encoded{};
  for (size_t i = 0; i < encoded.size(); ++i) {
    const unsigned shift = static_cast<unsigned>((encoded.size() - 1 - i) * 4);
    encoded[i] = HEX_DIGITS[(hash >> shift) & 0x0FU];
  }
  return getBookmarksDir() + "book_" + std::string(encoded.data(), encoded.size()) + ".json";
}

std::string BookmarkUtil::getLegacyBookmarkPath(const std::string& bookPath) {
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

bool BookmarkUtil::isEmptyBookmarkFile(const std::string& path) {
  constexpr char EMPTY_BOOKMARKS[] = "{\"bookmarks\":[]}";
  HalFile file;
  std::array<char, sizeof(EMPTY_BOOKMARKS) - 1> bytes{};
  if (!Storage.openFileForRead("BKM", path, file) || file.fileSize64() != bytes.size() ||
      file.read(bytes.data(), bytes.size()) != static_cast<int>(bytes.size())) {
    return false;
  }
  return std::memcmp(bytes.data(), EMPTY_BOOKMARKS, bytes.size()) == 0;
}

bool BookmarkUtil::writeEmptyBookmarkFile(const std::string& path) {
  constexpr char EMPTY_BOOKMARKS[] = "{\"bookmarks\":[]}";
  HalFile file;
  if (!Storage.openFileForWrite("BKM", path, file)) return false;
  const bool written = file.write(EMPTY_BOOKMARKS, sizeof(EMPTY_BOOKMARKS) - 1) == sizeof(EMPTY_BOOKMARKS) - 1;
  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!written || !synced || !closed) return false;
  return isEmptyBookmarkFile(path);
}

bool BookmarkUtil::writeEmptyCanonicalBookmark(const std::string& bookPath) {
  constexpr char ROOT[] = "/.crosspoint";
  const std::string directory = getBookmarksDir();
  if ((!Storage.exists(ROOT) && !Storage.mkdir(ROOT)) ||
      (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str()))) {
    return false;
  }
  return writeEmptyBookmarkFile(getBookmarkPath(bookPath));
}

bool BookmarkUtil::quarantineCanonicalForReplacement(const std::string& bookPath, const std::string& cachePath) {
  constexpr char ROOT[] = "/.crosspoint";
  // A replacement is rare; 256 preserved generations is generous while
  // bounding SD exists() calls if the archive namespace is pathological.
  constexpr unsigned MAX_ORPHAN_SLOTS = 256;
  if (bookPath.empty() || cachePath.empty() || cachePath == "/" || !Storage.exists(cachePath.c_str())) return false;

  const std::string canonical = getBookmarkPath(bookPath);
  const std::string pending = canonical + ".crossvi_replacement.tmp";
  if (Storage.exists(canonical.c_str()) && !isEmptyBookmarkFile(canonical)) {
    std::string destination;
    for (unsigned slot = 0; slot < MAX_ORPHAN_SLOTS; ++slot) {
      std::string candidate = cachePath + "/.crossvi_replaced_bookmark.json";
      if (slot > 0) candidate += "." + std::to_string(slot + 1);
      if (!Storage.exists(candidate.c_str())) {
        destination = std::move(candidate);
        break;
      }
    }
    if (destination.empty() || !Storage.rename(canonical.c_str(), destination.c_str())) return false;
  }

  if (Storage.exists(canonical.c_str())) {
    if (!isEmptyBookmarkFile(canonical)) return false;
    if (!Storage.exists(pending.c_str())) return true;
    // The verified canonical tombstone is authoritative. Any leftover pending
    // file, including a truncated one, is unpublished and safe to discard.
    return Storage.remove(pending.c_str());
  }

  const std::string directory = getBookmarksDir();
  if ((!Storage.exists(ROOT) && !Storage.mkdir(ROOT)) ||
      (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str()))) {
    return false;
  }
  if (Storage.exists(pending.c_str())) {
    // This name is exclusively our unpublished tombstone. A short write or
    // power loss may leave it truncated; the archived canonical bookmark and
    // source-identity barrier remain authoritative, so rebuilding the temp is
    // both safe and necessary for retry to make progress.
    if (!isEmptyBookmarkFile(pending) && !Storage.remove(pending.c_str())) return false;
    if (!Storage.exists(pending.c_str()) && !writeEmptyBookmarkFile(pending)) return false;
  } else if (!writeEmptyBookmarkFile(pending)) {
    return false;
  }
  return Storage.rename(pending.c_str(), canonical.c_str()) && isEmptyBookmarkFile(canonical);
}

bool BookmarkUtil::ensureLegacyBookmarkShadowed(const std::string& bookPath) {
  if (!Storage.exists(getLegacyBookmarkPath(bookPath).c_str())) return true;
  return writeEmptyCanonicalBookmark(bookPath);
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
  if (summary.size() > 72) {
    summary.resize(72);
  }
  return summary;
}
