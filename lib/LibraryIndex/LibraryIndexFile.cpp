#include "LibraryIndexFile.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace library {

LibraryIndexFile::~LibraryIndexFile() { close(); }

bool LibraryIndexFile::open(const char* path) {
  close();
  file = new (std::nothrow) HalFile();
  if (file == nullptr) {
    LOG_ERR("LIBIDX", "HalFile alloc failed");
    return false;
  }
  if (!Storage.openFileForRead("LIBIDX", path, *file)) {
    delete file;
    file = nullptr;
    return false;
  }

  if (file->read(&head, sizeof(head)) != static_cast<int>(sizeof(head))) {
    lastValidity = ClixValidity::SizeMismatch;
    file->close();
    delete file;
    file = nullptr;
    return false;
  }

  lastValidity = validateHeader(head, file->fileSize64());
  if (lastValidity != ClixValidity::Ok) {
    LOG_INF("LIBIDX", "index rejected: %s", clixValidityName(lastValidity));
    file->close();
    delete file;
    file = nullptr;
    return false;
  }
  opened = true;
  return true;
}

void LibraryIndexFile::close() {
  if (file != nullptr) {
    file->close();
    delete file;
    file = nullptr;
  }
  opened = false;
}

bool LibraryIndexFile::readAt(const uint32_t offset, void* dst, const size_t len) {
  if (!opened || file == nullptr) return false;
  // Every offset handed to this function comes from the header, and the header
  // was validated against the real file size, so a short read means the card
  // changed under us rather than a bad computation.
  if (!file->seekSet(offset)) return false;
  return file->read(dst, len) == static_cast<int>(len);
}

uint16_t LibraryIndexFile::ordinalForRow(const SortOrder order, const uint16_t row) {
  constexpr uint16_t NONE = 0xFFFF;
  if (!opened || row >= head.bookCount) return NONE;

  switch (order) {
    case SortOrder::TitleAsc:
      // The record section IS in title order, so this costs no storage and no
      // read at all.
      return row;
    case SortOrder::TitleDesc:
      return static_cast<uint16_t>(head.bookCount - 1 - row);
    case SortOrder::AuthorAsc: {
      uint16_t ordinal = NONE;
      return readAt(authorOrderOffset(head, row), &ordinal, sizeof(ordinal)) ? ordinal : NONE;
    }
    case SortOrder::DateDesc: {
      // dateOrder runs oldest first, so newest-first is the same array read
      // backwards — no second array, no second sort.
      const uint16_t k = static_cast<uint16_t>(head.bookCount - 1 - row);
      uint16_t ordinal = NONE;
      return readAt(dateOrderOffset(head, k), &ordinal, sizeof(ordinal)) ? ordinal : NONE;
    }
  }
  return NONE;
}

bool LibraryIndexFile::readRecord(const uint16_t ordinal, ClixRecord& out) {
  if (!opened || ordinal >= head.bookCount) return false;
  if (!readAt(recordOffset(head, ordinal), &out, sizeof(out))) return false;

  // Clamp here, at the single point every record enters the program. These
  // lengths come off an SD card that the user can write to and that can rot: a
  // foldLen of 255 against a 96-byte field sends a string_view 159 bytes past the
  // end of the record, and callers build views from them without looking. Fixing
  // it at each call site would mean fixing it again at the next one.
  out.foldLen = static_cast<uint8_t>(std::min<size_t>(out.foldLen, CLIX_FOLD_BYTES));
  out.authorKeyLen = static_cast<uint8_t>(std::min<size_t>(out.authorKeyLen, CLIX_AUTHOR_KEY_BYTES));
  // nameOff is u32 and every reader adds a length to it before comparing against
  // the section size. A forged value near the top of the range wraps that sum and
  // passes the bounds check it was supposed to fail, so it is rejected here
  // instead — the one place that can, before any arithmetic touches it.
  if (out.nameOff > head.nameLen) {
    out.nameLen = 0;
    out.nameOff = 0;
  }
  return true;
}

bool LibraryIndexFile::readName(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  if (record.nameOff + record.nameLen > head.nameLen) return false;
  out.resize(record.nameLen);
  return readAt(head.nameStart + record.nameOff, out.data(), record.nameLen);
}

bool LibraryIndexFile::readAuthor(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  const uint32_t lenAt = record.nameOff + record.nameLen;
  if (lenAt + 1 > head.nameLen) return false;

  uint8_t authorLen = 0;
  if (!readAt(head.nameStart + lenAt, &authorLen, sizeof(authorLen))) return false;
  if (authorLen == 0) return false;
  if (lenAt + 1 + authorLen > head.nameLen) return false;

  out.resize(authorLen);
  return readAt(head.nameStart + lenAt + 1, out.data(), authorLen);
}

// The book's own title, after the name and the author. Absent (length 0) for a
// book that never told us one, in which case the caller shows the filename.
bool LibraryIndexFile::readTitle(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.nameLen == 0) return false;
  uint32_t at = record.nameOff + record.nameLen;
  if (at + 1 > head.nameLen) return false;

  uint8_t authorLen = 0;
  if (!readAt(head.nameStart + at, &authorLen, sizeof(authorLen))) return false;
  at += 1 + authorLen;
  if (at + 1 > head.nameLen) return false;

  uint8_t titleLen = 0;
  if (!readAt(head.nameStart + at, &titleLen, sizeof(titleLen))) return false;
  if (titleLen == 0 || at + 1 + titleLen > head.nameLen) return false;

  out.resize(titleLen);
  return readAt(head.nameStart + at + 1, out.data(), titleLen);
}

bool LibraryIndexFile::readPath(const ClixRecord& record, std::string& out) {
  out.clear();
  if (!opened || record.folderId >= head.folderCount) return false;

  // Folder records are variable length, so reaching folder n means walking the
  // n preceding length bytes. At one seek per folder this is only done when a
  // book is opened or its details are shown, never while paging.
  uint32_t offset = head.folderStart;
  for (uint16_t i = 0; i <= record.folderId; i++) {
    uint8_t pathLen = 0;
    if (!readAt(offset, &pathLen, sizeof(pathLen)) || pathLen == 0) return false;
    if (i == record.folderId) {
      std::string dir(pathLen, '\0');
      if (!readAt(offset + 1, dir.data(), pathLen)) return false;
      std::string name;
      if (!readName(record, name)) return false;
      out = dir + "/" + name;
      return true;
    }
    offset += 1u + pathLen;
    if (offset >= head.folderStart + head.folderLen) return false;
  }
  return false;
}

}  // namespace library
