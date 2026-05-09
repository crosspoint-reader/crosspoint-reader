#include "EpubBookmarksStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

namespace {

std::string bookmarksPath(const Epub& epub) { return epub.getCachePath() + "/bookmarks.bin"; }

inline uint16_t readU16LE(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }

inline void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

}  // namespace

namespace EpubBookmarksStore {

bool load(const Epub& epub, std::vector<EpubBookmark>& outBookmarks) {
  outBookmarks.clear();
  outBookmarks.reserve(MAX_BOOKMARKS);

  FsFile f;
  const std::string path = bookmarksPath(epub);
  if (!Storage.openFileForRead("BKM", path, f)) {
    // Missing file is fine: no bookmarks yet.
    return true;
  }

  uint8_t hdr[3];
  if (f.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
    LOG_ERR("BKM", "Short read bookmarks header");
    return false;
  }

  const uint8_t version = hdr[0];
  if (version != BOOKMARKS_VERSION) {
    LOG_ERR("BKM", "Unsupported bookmarks version: %u", static_cast<unsigned>(version));
    return false;
  }

  const uint16_t count = readU16LE(&hdr[1]);
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKM", "Bookmarks count too large: %u", static_cast<unsigned>(count));
    return false;
  }

  for (uint16_t i = 0; i < count; i++) {
    uint8_t entry[4];
    if (f.read(entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) {
      LOG_ERR("BKM", "Short read bookmarks entry %u", static_cast<unsigned>(i));
      return false;
    }
    EpubBookmark b;
    b.spineIndex = readU16LE(&entry[0]);
    b.pageNumber = readU16LE(&entry[2]);
    outBookmarks.push_back(b);
  }

  return true;
}

bool save(const Epub& epub, const std::vector<EpubBookmark>& bookmarks) {
  const size_t count = std::min(bookmarks.size(), MAX_BOOKMARKS);
  FsFile f;
  const std::string path = bookmarksPath(epub);
  if (!Storage.openFileForWrite("BKM", path, f)) {
    LOG_ERR("BKM", "Failed to open bookmarks file for write");
    return false;
  }

  uint8_t hdr[3];
  hdr[0] = BOOKMARKS_VERSION;
  writeU16LE(&hdr[1], static_cast<uint16_t>(count));
  const size_t hdrWritten = f.write(hdr, sizeof(hdr));
  if (hdrWritten != sizeof(hdr)) {
    LOG_ERR("BKM", "Short write bookmarks header: %u/%u", (unsigned)hdrWritten, (unsigned)sizeof(hdr));
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    uint8_t entry[4];
    writeU16LE(&entry[0], bookmarks[i].spineIndex);
    writeU16LE(&entry[2], bookmarks[i].pageNumber);
    const size_t written = f.write(entry, sizeof(entry));
    if (written != sizeof(entry)) {
      LOG_ERR("BKM", "Short write bookmarks entry %u: %u/%u", (unsigned)i, (unsigned)written, (unsigned)sizeof(entry));
      return false;
    }
  }

  return true;
}

bool contains(const Epub& epub, const uint16_t spineIndex, const uint16_t pageNumber) {
  std::vector<EpubBookmark> bookmarks;
  if (!load(epub, bookmarks)) {
    return false;
  }
  return std::any_of(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const EpubBookmark& b) {
    return b.spineIndex == spineIndex && b.pageNumber == pageNumber;
  });
}

bool add(const Epub& epub, const uint16_t spineIndex, const uint16_t pageNumber) {
  std::vector<EpubBookmark> bookmarks;
  if (!load(epub, bookmarks)) {
    return false;
  }
  if (std::any_of(bookmarks.begin(), bookmarks.end(), [spineIndex, pageNumber](const EpubBookmark& b) {
        return b.spineIndex == spineIndex && b.pageNumber == pageNumber;
      })) {
    return true;
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    // Keep newest by dropping oldest.
    bookmarks.erase(bookmarks.begin());
  }
  bookmarks.push_back(EpubBookmark{spineIndex, pageNumber});
  return save(epub, bookmarks);
}

bool remove(const Epub& epub, const uint16_t spineIndex, const uint16_t pageNumber) {
  std::vector<EpubBookmark> bookmarks;
  if (!load(epub, bookmarks)) {
    return false;
  }

  const auto before = bookmarks.size();
  bookmarks.erase(std::remove_if(bookmarks.begin(), bookmarks.end(),
                                 [spineIndex, pageNumber](const EpubBookmark& b) {
                                   return b.spineIndex == spineIndex && b.pageNumber == pageNumber;
                                 }),
                  bookmarks.end());

  if (bookmarks.size() == before) {
    return true;  // nothing to do
  }
  return save(epub, bookmarks);
}

}  // namespace EpubBookmarksStore
