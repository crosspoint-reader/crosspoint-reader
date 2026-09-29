#include "ClippingStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <functional>

#include "clippings/ClippingPreview.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t LAYOUT_VERSION = 3;
constexpr uint8_t VERSION = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr size_t TEXT_COPY_BUFFER_SIZE = 128;
constexpr size_t HEADER_STRING_MAX = CLIPPING_TEXT_MAX;

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(std::hash<std::string>{}(filePath)) +
         ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
  dst[utf8SafeTruncateBuffer(dst, static_cast<int>(strlen(dst)))] = '\0';
}

bool copyBytes(HalFile& in, HalFile& out, uint16_t length) {
  std::array<uint8_t, TEXT_COPY_BUFFER_SIZE> buffer{};
  while (length > 0) {
    const size_t chunk = std::min<size_t>(length, buffer.size());
    if (in.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      return false;
    }
    if (out.write(buffer.data(), chunk) != chunk) {
      return false;
    }
    length = static_cast<uint16_t>(length - chunk);
  }
  return true;
}
constexpr const char* STORE_SUFFIXES[] = {"", ".bak", ".tmp", ".deleted", ".migrate.bak"};
enum class MovePhase { Check, Move, Rollback };

bool moveStoreFiles(const std::string& from, const std::string& to, const MovePhase phase) {
  const std::string source = storeFilePathForBook(from, "epub");
  const std::string target = storeFilePathForBook(to, "epub");
  if (source == target) return true;
  return std::all_of(std::begin(STORE_SUFFIXES), std::end(STORE_SUFFIXES), [&](const char* suffix) {
    const std::string oldPath = source + suffix;
    const std::string newPath = target + suffix;
    if (phase == MovePhase::Check) {
      if (Storage.exists(newPath.c_str())) {
        LOG_ERR("CLIP", "Move would overwrite saved clippings: %s", to.c_str());
        return false;
      }
    } else {
      const auto& src = phase == MovePhase::Move ? oldPath : newPath;
      const auto& dst = phase == MovePhase::Move ? newPath : oldPath;
      if (Storage.exists(src.c_str()) && !Storage.rename(src.c_str(), dst.c_str())) {
        LOG_ERR("CLIP", "Failed moving clipping state: %s -> %s", src.c_str(), dst.c_str());
        return false;
      }
    }
    return true;
  });
}

struct MoveDirectory {
  HalFile file;
  std::string from;
  std::string to;
};
// ponytail: cap directory depth at 32; grow this checked work stack if deeper book folders are needed.
constexpr size_t MAX_MOVE_DEPTH = 32;

bool moveTreeStores(const std::string& from, const std::string& to, const MovePhase phase, MoveDirectory* stack,
                    char* name) {
  for (size_t i = 0; i < MAX_MOVE_DEPTH; ++i) {
    if (stack[i].file) stack[i].file.close();
  }
  HalFile root = Storage.open(from.c_str());
  if (!root || !root.isDirectory()) return false;
  stack[0] = {std::move(root), from, to};
  size_t depth = 1;
  while (depth) {
    auto& directory = stack[depth - 1];
    HalFile entry = directory.file.openNextFile();
    if (!entry) {
      directory.file.close();
      --depth;
      continue;
    }
    const size_t length = entry.getName(name, 1024);
    if (length == 0 || length >= 1024) {
      LOG_ERR("CLIP", "Invalid name in folder move");
      return false;
    }
    if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
    const std::string oldPath = directory.from + (directory.from.back() == '/' ? "" : "/") + name;
    const std::string newPath = directory.to + (directory.to.back() == '/' ? "" : "/") + name;
    if (entry.isDirectory()) {
      if (depth == MAX_MOVE_DEPTH) {
        LOG_ERR("CLIP", "Folder move exceeds directory depth limit");
        return false;
      }
      stack[depth++] = {std::move(entry), oldPath, newPath};
    } else if (!moveStoreFiles(oldPath, newPath, phase)) {
      return false;
    }
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  dirty = false;
  loaded = false;
  for (auto& block : clippingBlocks) block.reset();
  clippingSize = clippingCapacity = 0;
  storeFilePath.clear();
  if (bookType != "epub" || filePath.empty()) {
    LOG_ERR("CLIP", "Invalid clipping book type or path: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;

  storeFilePath = storeFilePathForBook(filePath, bookType);
  const std::string backup = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to recover clipping backup");
    return false;
  }
  if (!Storage.exists(storeFilePath.c_str())) {
    loaded = true;
    return true;
  }
  loaded = readFromFile();
  if (!loaded) {
    for (auto& block : clippingBlocks) block.reset();
    clippingSize = clippingCapacity = 0;
  }
  return loaded;
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  for (auto& block : clippingBlocks) block.reset();
  clippingSize = clippingCapacity = 0;
  loaded = false;
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text,
                                                    const uint32_t layoutSignature, const uint32_t startOffset,
                                                    const uint32_t endOffset) {
  if (clippingSize >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  if (!loaded || text.size() > CLIPPING_TEXT_MAX || !reserveClippings(clippingSize + 1)) {
    LOG_ERR("CLIP", "Cannot add clipping: invalid store, text length, or allocation failure");
    return AddResult::SaveFailed;
  }
  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  const time_t now = time(nullptr);
  clipping.timestamp = now > 1577836800 ? static_cast<uint32_t>(now) : 0;
  clipping.layoutSignature = layoutSignature;
  clipping.startOffset = startOffset;
  clipping.endOffset = endOffset;
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  clipping.textLength = static_cast<uint16_t>(std::min(text.size(), CLIPPING_TEXT_MAX));

  clippingRef(clippingSize++) = clipping;
  dirty = true;
  if (!writeToFile(&text, clippingSize - 1)) {
    --clippingSize;
    dirty = true;
    return AddResult::SaveFailed;
  }
  dirty = false;
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (!loaded || index >= clippingSize) return false;
  if (clippingRef(index).id[0]) {
    HalFile journal = Storage.open((storeFilePath + ".deleted").c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!journal || journal.write(reinterpret_cast<const uint8_t*>(clippingRef(index).id),
                                  sizeof(clippingRef(index).id)) != sizeof(clippingRef(index).id)) {
      LOG_ERR("CLIP", "Failed to queue clipping deletion");
      return false;
    }
    journal.flush();
  }
  Clipping clipping = std::move(clippingRef(index));
  for (size_t i = index; i + 1 < clippingSize; ++i) clippingRef(i) = clippingRef(i + 1);
  --clippingSize;
  dirty = true;
  if (!saveToFile()) {
    for (size_t i = clippingSize; i > index; --i) clippingRef(i) = clippingRef(i - 1);
    clippingRef(index) = clipping;
    ++clippingSize;
    dirty = true;
    return false;
  }
  return true;
}

const Clipping* ClippingStore::clippingAt(const size_t index) const {
  if (index >= clippingSize) return nullptr;
  return &clippingBlocks[index / CLIPPINGS_PER_BLOCK][index % CLIPPINGS_PER_BLOCK];
}

bool ClippingStore::readClippingPreview(const size_t index, char* out, const size_t outSize, size_t& outLength) const {
  outLength = 0;
  if (out && outSize > 0) out[0] = '\0';
  const Clipping* clipping = clippingAt(index);
  if (!out || outSize == 0 || !clipping || storeFilePath.empty()) {
    LOG_ERR("CLIP", "Invalid clipping preview index: %u", static_cast<unsigned>(index));
    return false;
  }
  if (clipping->textLength == 0) return true;

  HalFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) return false;
  if (!f.seek(clipping->textOffset)) {
    LOG_ERR("CLIP", "Failed to seek clipping preview at %u", clipping->textOffset);
    return false;
  }
  const bool ok = clippingPreview::read(f, clipping->textLength, out, outSize, outLength);
  if (!ok) LOG_ERR("CLIP", "Failed to read clipping preview at %u", clipping->textOffset);
  return ok;
}

bool ClippingStore::readClippingText(const size_t index, std::string& out) const {
  const Clipping* clipping = clippingAt(index);
  if (!clipping) return false;
  return readClippingText(*clipping, out);
}

bool ClippingStore::readClippingText(const Clipping& clipping, std::string& out) const {
  out.clear();
  if (clipping.textLength == 0) return true;
  if (storeFilePath.empty()) return false;

  HalFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) {
    return false;
  }
  if (!f.seek(clipping.textOffset)) {
    LOG_ERR("CLIP", "Failed to seek clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
    return false;
  }
  out.resize(clipping.textLength);
  const int expected = static_cast<int>(clipping.textLength);
  const bool ok = f.read(&out[0], clipping.textLength) == expected;
  if (!ok) {
    out.clear();
    LOG_ERR("CLIP", "Failed to read clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
  }
  return ok;
}

bool ClippingStore::saveToFile() {
  if (!loaded) return false;
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

bool ClippingStore::prepareSync() {
  for (size_t index = 0; index < clippingSize; ++index) {
    Clipping& clipping = clippingRef(index);
    if (clipping.id[0]) continue;
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      clipping.id[i * 2] = HEX_DIGITS[bytes[i] >> 4];
      clipping.id[i * 2 + 1] = HEX_DIGITS[bytes[i] & 15];
    }
    clipping.id[32] = '\0';
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::finishUploads() {
  for (size_t index = 0; index < clippingSize; ++index) {
    Clipping& clipping = clippingRef(index);
    if (!clipping.pendingUpload) continue;
    clipping.pendingUpload = false;
    dirty = true;
  }
  return saveToFile();
}

bool ClippingStore::nextDeletion(uint32_t& offset, char (&id)[65]) const {
  id[0] = '\0';
  const std::string path = storeFilePath + ".deleted";
  if (!Storage.exists(path.c_str())) return true;
  HalFile journal;
  if (!Storage.openFileForRead("CLIP", path, journal)) return false;
  if (journal.size() % sizeof(id) != 0 || !journal.seek(offset)) {
    LOG_ERR("CLIP", "Invalid clipping deletion journal");
    return false;
  }
  while (offset < journal.size()) {
    if (journal.read(reinterpret_cast<uint8_t*>(id), sizeof(id)) != sizeof(id)) {
      LOG_ERR("CLIP", "Failed to read clipping deletion");
      return false;
    }
    offset += sizeof(id);
    id[sizeof(id) - 1] = '\0';
    size_t index = 0;
    while (index < clippingSize && strcmp(clippingAt(index)->id, id) != 0) ++index;
    if (index == clippingSize) return true;
  }
  id[0] = '\0';
  return true;
}

bool ClippingStore::finishDeletions() {
  const std::string path = storeFilePath + ".deleted";
  if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
  LOG_ERR("CLIP", "Failed to clear acknowledged deletions");
  return false;
}

bool ClippingStore::applyRemote(const Clipping& clipping, const std::string& text, const bool deleted) {
  if (!loaded || (!deleted && text.size() > CLIPPING_TEXT_MAX)) return false;
  size_t index = 0;
  while (index < clippingSize && strcmp(clippingRef(index).id, clipping.id) != 0) ++index;
  if (index == clippingSize) {
    if (deleted) return true;
    if (clippingSize >= CLIPPING_MAX_PER_BOOK || !reserveClippings(clippingSize + 1)) {
      LOG_ERR("CLIP", "Remote clipping exceeds local capacity");
      return false;
    }
    clippingRef(clippingSize++) = clipping;
    if (!writeToFile(&text, index)) {
      --clippingSize;
      return false;
    }
    return true;
  }
  if (!deleted && clippingRef(index).syncRevision == clipping.syncRevision && !clippingRef(index).pendingUpload)
    return true;
  const Clipping previous = clippingRef(index);
  if (deleted) {
    for (size_t i = index; i + 1 < clippingSize; ++i) clippingRef(i) = clippingRef(i + 1);
    --clippingSize;
  } else {
    clippingRef(index) = clipping;
  }
  if (writeToFile(deleted ? nullptr : &text, index)) return true;
  if (deleted) {
    for (size_t i = clippingSize; i > index; --i) clippingRef(i) = clippingRef(i - 1);
    ++clippingSize;
  }
  clippingRef(index) = previous;
  return false;
}

bool ClippingStore::reserveClippings(const size_t count) {
  if (count > CLIPPING_MAX_PER_BOOK) return false;
  while (clippingCapacity < count) {
    auto block = makeUniqueNoThrow<Clipping[]>(CLIPPINGS_PER_BLOCK);
    if (!block) {
      LOG_ERR("CLIP", "OOM: clipping block (%u bytes)", static_cast<unsigned>(CLIPPINGS_PER_BLOCK * sizeof(Clipping)));
      return false;
    }
    clippingBlocks[clippingCapacity / CLIPPINGS_PER_BLOCK] = std::move(block);
    clippingCapacity += CLIPPINGS_PER_BLOCK;
  }
  return true;
}

bool ClippingStore::readFromFile() {
  const auto& path = storeFilePath;
  clippingSize = 0;
  HalFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) || (version < LEGACY_VERSION || version > VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, title, HEADER_STRING_MAX) ||
      !serialization::tryReadString(f, author, HEADER_STRING_MAX) ||
      !serialization::tryReadString(f, storedPath, HEADER_STRING_MAX)) {
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    return false;
  }

  if (bookTitle.empty()) bookTitle = std::move(title);
  if (bookAuthor.empty()) bookAuthor = std::move(author);
  if (!reserveClippings(count)) return false;
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= LAYOUT_VERSION && !serialization::tryReadPod(f, clipping.layoutSignature)) {
      LOG_ERR("CLIP", "Clipping file truncated at layout signature, record %u: %s", i, path.c_str());
      return false;
    }
    if (version >= VERSION &&
        (!serialization::tryReadPod(f, clipping.startOffset) || !serialization::tryReadPod(f, clipping.endOffset) ||
         !serialization::tryReadPod(f, clipping.syncRevision) ||
         !serialization::tryReadPod(f, clipping.pendingUpload) ||
         f.read(reinterpret_cast<uint8_t*>(clipping.id), sizeof(clipping.id)) != sizeof(clipping.id))) {
      LOG_ERR("CLIP", "Truncated clipping sync metadata");
      return false;
    }
    clipping.id[sizeof(clipping.id) - 1] = '\0';
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    if (version == LEGACY_VERSION) {
      uint32_t textLen = 0;
      if (!serialization::tryReadPod(f, textLen)) {
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      clipping.textLength = static_cast<uint16_t>(std::min<uint32_t>(textLen, CLIPPING_TEXT_MAX));
      if (textLen > static_cast<uint32_t>(f.available()) || (textLen > 0 && !f.seekCur(textLen))) {
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    } else {
      if (!serialization::tryReadPod(f, clipping.textLength)) {
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      if (clipping.textLength > CLIPPING_TEXT_MAX) {
        LOG_ERR("CLIP", "Clipping text length %u exceeds max, record %u: %s", clipping.textLength, i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      if (clipping.textLength > f.available() || (clipping.textLength > 0 && !f.seekCur(clipping.textLength))) {
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    }
    clippingRef(clippingSize++) = clipping;
  }

  return true;
}

bool ClippingStore::writeToFile(const std::string* replacementText, const size_t replacementIndex) {
  if (!loaded || storeFilePath.empty() || bookTitle.size() > HEADER_STRING_MAX ||
      bookAuthor.size() > HEADER_STRING_MAX || bookFilePath.size() > HEADER_STRING_MAX ||
      clippingSize > CLIPPING_MAX_PER_BOOK || (replacementText && replacementText->size() > CLIPPING_TEXT_MAX)) {
    LOG_ERR("CLIP", "Refusing to write invalid or incompletely loaded clipping store");
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(clippingSize);
  auto newTextOffsets = makeUniqueNoThrow<uint32_t[]>(count);
  if (count && !newTextOffsets) {
    LOG_ERR("CLIP", "OOM: clipping text offsets");
    return false;
  }
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  const std::string tmpPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered clipping backup: %s", storeFilePath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(backupPath.c_str()) && Storage.exists(storeFilePath.c_str())) Storage.remove(backupPath.c_str());

  HalFile source;
  const std::string& sourcePath = storeFilePath;
  const bool hasTextSource = Storage.exists(sourcePath.c_str());
  const bool hasDestination = Storage.exists(storeFilePath.c_str());
  if (hasTextSource && !Storage.openFileForRead("CLIP", sourcePath, source)) {
    LOG_ERR("CLIP", "Failed to open clipping source for rewrite: %s", sourcePath.c_str());
    return false;
  }

  HalFile f = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (source) source.close();
    LOG_ERR("CLIP", "Failed to open clipping temp file for write: %s", tmpPath.c_str());
    return false;
  }

  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippingRef(i);
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        !serialization::tryWritePod(f, clipping.layoutSignature) ||
        !serialization::tryWritePod(f, clipping.startOffset) || !serialization::tryWritePod(f, clipping.endOffset) ||
        !serialization::tryWritePod(f, clipping.syncRevision) ||
        !serialization::tryWritePod(f, clipping.pendingUpload) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.id), sizeof(clipping.id)) != sizeof(clipping.id) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = replacementText && i == replacementIndex;
    const uint16_t textLen = useReplacement
                                 ? static_cast<uint16_t>(std::min(replacementText->size(), CLIPPING_TEXT_MAX))
                                 : clipping.textLength;
    if (!serialization::tryWritePod(f, textLen)) {
      LOG_ERR("CLIP", "Failed to write clipping text length %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const uint32_t newTextOffset = static_cast<uint32_t>(f.position());
    bool wroteText = true;
    if (textLen > 0 && useReplacement) {
      wroteText = f.write(reinterpret_cast<const uint8_t*>(replacementText->data()), textLen) == textLen;
    } else if (textLen > 0) {
      wroteText = source && source.seek(clipping.textOffset) && copyBytes(source, f, textLen);
    }
    if (!wroteText) {
      LOG_ERR("CLIP", "Failed to write clipping text %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    newTextOffsets[i] = newTextOffset;
  }

  f.flush();
  f.close();
  if (source) source.close();

  if (hasDestination && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to replace clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    if (hasDestination) Storage.rename(backupPath.c_str(), storeFilePath.c_str());
    return false;
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    clippingRef(i).textOffset = newTextOffsets[i];
  }
  return true;
}

bool ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  bool ok = true;
  for (const char* suffix : STORE_SUFFIXES) {
    const std::string sidecar = path + suffix;
    if (Storage.exists(sidecar.c_str()) && !Storage.remove(sidecar.c_str())) {
      LOG_ERR("CLIP", "Failed deleting clipping state: %s", sidecar.c_str());
      ok = false;
    }
  }
  return ok;
}

bool ClippingStore::moveBook(const std::string& from, const std::string& to) {
  if (from == to) return true;
  if (Storage.exists(to.c_str())) return false;
  bool directory = false;
  {
    HalFile root = Storage.open(from.c_str());
    if (!root) return false;
    directory = root.isDirectory();
  }
  // Reuse one checked heap workspace for preflight, move, and rollback.
  auto stack = directory ? makeUniqueNoThrow<MoveDirectory[]>(MAX_MOVE_DEPTH) : nullptr;
  auto name = directory ? makeUniqueNoThrow<char[]>(1024) : nullptr;
  if (directory && (!stack || !name)) {
    LOG_ERR("CLIP", "OOM: folder move work stack");
    return false;
  }
  const auto move = [&](const MovePhase phase) {
    return directory ? moveTreeStores(from, to, phase, stack.get(), name.get()) : moveStoreFiles(from, to, phase);
  };
  if (!move(MovePhase::Check)) return false;
  if (move(MovePhase::Move) && Storage.rename(from.c_str(), to.c_str())) return true;
  if (!move(MovePhase::Rollback)) {
    LOG_ERR("CLIP", "Book move rollback failed: %s -> %s", from.c_str(), to.c_str());
  }
  return false;
}
