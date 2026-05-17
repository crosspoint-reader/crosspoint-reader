#include "IncrementalSectionCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <common/FsApiConstants.h>

#include <limits>
#include <utility>

#include "Page.h"

namespace IncrementalSection {
namespace {

constexpr uint32_t MAX_ANCHOR_KEY_LEN = 1024;

bool ensureDirectory(const std::string& path) {
  if (Storage.exists(path.c_str())) {
    return true;
  }
  return Storage.mkdir(path.c_str());
}

bool checkedWriteBytes(FsFile& file, const uint8_t* data, const size_t size, const char* label) {
  if (size == 0) {
    return true;
  }
  const size_t written = file.write(data, size);
  if (written != size) {
    LOG_ERR("ISC", "Short write %s: %u/%u bytes", label, static_cast<unsigned>(written), static_cast<unsigned>(size));
    return false;
  }
  return true;
}

template <typename T>
bool checkedWritePod(FsFile& file, const T& value, const char* label) {
  return checkedWriteBytes(file, reinterpret_cast<const uint8_t*>(&value), sizeof(T), label);
}

bool checkedWriteString(FsFile& file, const std::string& value, const char* label) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    LOG_ERR("ISC", "String too large for %s: %u bytes", label, static_cast<unsigned>(value.size()));
    return false;
  }
  const uint32_t length = static_cast<uint32_t>(value.size());
  return checkedWritePod(file, length, label) &&
         checkedWriteBytes(file, reinterpret_cast<const uint8_t*>(value.data()), length, label);
}

bool readAnchorEntry(FsFile& file, std::string& key, uint16_t& page) {
  if (file.available() < static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t))) {
    LOG_ERR("ISC", "Truncated anchor entry");
    return false;
  }

  uint32_t length = 0;
  serialization::readPod(file, length);
  const int remaining = file.available();
  if (remaining < static_cast<int>(sizeof(uint16_t)) ||
      length > static_cast<uint32_t>(remaining - static_cast<int>(sizeof(uint16_t))) || length > MAX_ANCHOR_KEY_LEN) {
    LOG_ERR("ISC", "Invalid anchor key length: %lu", static_cast<unsigned long>(length));
    return false;
  }

  key.resize(length);
  if (length > 0) {
    const size_t read = file.read(reinterpret_cast<uint8_t*>(&key[0]), length);
    if (read != length) {
      LOG_ERR("ISC", "Short read anchor key: %u/%lu bytes", static_cast<unsigned>(read),
              static_cast<unsigned long>(length));
      return false;
    }
  }
  serialization::readPod(file, page);
  return true;
}

bool checkedWriteLayoutKey(FsFile& file, const LayoutCacheKey& key) {
  return checkedWritePod(file, key.cacheVersion, "layout.cacheVersion") &&
         checkedWritePod(file, key.fontId, "layout.fontId") &&
         checkedWritePod(file, key.lineCompression, "layout.lineCompression") &&
         checkedWritePod(file, key.extraParagraphSpacing, "layout.extraParagraphSpacing") &&
         checkedWritePod(file, key.paragraphAlignment, "layout.paragraphAlignment") &&
         checkedWritePod(file, key.viewportWidth, "layout.viewportWidth") &&
         checkedWritePod(file, key.viewportHeight, "layout.viewportHeight") &&
         checkedWritePod(file, key.hyphenationEnabled, "layout.hyphenationEnabled") &&
         checkedWritePod(file, key.embeddedStyle, "layout.embeddedStyle") &&
         checkedWritePod(file, key.imageRendering, "layout.imageRendering") &&
         checkedWritePod(file, key.focusReadingEnabled, "layout.focusReadingEnabled");
}

void readLayoutKey(FsFile& file, LayoutCacheKey& key) {
  serialization::readPod(file, key.cacheVersion);
  serialization::readPod(file, key.fontId);
  serialization::readPod(file, key.lineCompression);
  serialization::readPod(file, key.extraParagraphSpacing);
  serialization::readPod(file, key.paragraphAlignment);
  serialization::readPod(file, key.viewportWidth);
  serialization::readPod(file, key.viewportHeight);
  serialization::readPod(file, key.hyphenationEnabled);
  serialization::readPod(file, key.embeddedStyle);
  serialization::readPod(file, key.imageRendering);
  serialization::readPod(file, key.focusReadingEnabled);
}

bool checkedWriteIndexRecord(FsFile& file, const PageIndexRecord& record) {
  return checkedWritePod(file, record.pageOffset, "index.pageOffset") &&
         checkedWritePod(file, record.pageLength, "index.pageLength") &&
         checkedWritePod(file, record.paragraphIndex, "index.paragraphIndex") &&
         checkedWritePod(file, record.listItemIndex, "index.listItemIndex") &&
         checkedWritePod(file, record.sourceByteOffset, "index.sourceByteOffset");
}

void readIndexRecordFromFile(FsFile& file, PageIndexRecord& record) {
  serialization::readPod(file, record.pageOffset);
  serialization::readPod(file, record.pageLength);
  serialization::readPod(file, record.paragraphIndex);
  serialization::readPod(file, record.listItemIndex);
  serialization::readPod(file, record.sourceByteOffset);
}

bool readNextIndexRecord(FsFile& file, PageIndexRecord& record) {
  if (static_cast<size_t>(file.available()) < PAGE_INDEX_RECORD_SIZE) {
    return false;
  }
  readIndexRecordFromFile(file, record);
  return true;
}

}  // namespace

Cache::Cache(std::string sectionsDir, const uint32_t spineIndex)
    : sectionsDir_(std::move(sectionsDir)),
      spineIndex_(spineIndex),
      paths_(pathsForSection(sectionsDir_, spineIndex)) {}

bool Cache::loadMeta(Meta& out) const {
  FsFile file;
  if (!Storage.openFileForRead("ISC", paths_.meta, file)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  serialization::readPod(file, magic);
  serialization::readPod(file, version);
  if (magic != CACHE_MAGIC || version != CACHE_VERSION) {
    file.close();
    return false;
  }

  uint8_t state = 0;
  serialization::readPod(file, state);
  out.state = static_cast<CacheState>(state);
  serialization::readPod(file, out.spineIndex);
  readLayoutKey(file, out.layoutKey);
  serialization::readPod(file, out.sourceUncompressedSize);
  serialization::readPod(file, out.finalPageCount);
  serialization::readPod(file, out.anchorCount);
  serialization::readPod(file, out.generationId);
  const bool matchesSpine = out.spineIndex == spineIndex_;
  file.close();
  return matchesSpine;
}

bool Cache::writeMeta(const Meta& meta) const {
  FsFile file;
  if (!Storage.openFileForWrite("ISC", paths_.meta, file)) {
    return false;
  }

  const uint8_t state = static_cast<uint8_t>(meta.state);
  const bool success =
      checkedWritePod(file, CACHE_MAGIC, "meta.magic") && checkedWritePod(file, CACHE_VERSION, "meta.version") &&
      checkedWritePod(file, state, "meta.state") && checkedWritePod(file, meta.spineIndex, "meta.spineIndex") &&
      checkedWriteLayoutKey(file, meta.layoutKey) &&
      checkedWritePod(file, meta.sourceUncompressedSize, "meta.sourceUncompressedSize") &&
      checkedWritePod(file, meta.finalPageCount, "meta.finalPageCount") &&
      checkedWritePod(file, meta.anchorCount, "meta.anchorCount") &&
      checkedWritePod(file, meta.generationId, "meta.generationId");
  file.close();
  return success;
}

bool Cache::beginBuild(const LayoutCacheKey& key, const uint32_t sourceUncompressedSize, const uint32_t generationId) {
  removeAllFiles();
  if (!ensureDirectory(sectionsDir_)) {
    LOG_ERR("ISC", "Failed to create section cache dir: %s", sectionsDir_.c_str());
    return false;
  }

  FsFile pages;
  if (!Storage.openFileForWrite("ISC", paths_.pages, pages)) {
    removeAllFiles();
    return false;
  }
  pages.close();

  FsFile index;
  if (!Storage.openFileForWrite("ISC", paths_.index, index)) {
    removeAllFiles();
    return false;
  }
  index.close();

  FsFile anchors;
  if (!Storage.openFileForWrite("ISC", paths_.anchors, anchors)) {
    removeAllFiles();
    return false;
  }
  anchors.close();

  Meta meta;
  meta.state = CacheState::Building;
  meta.spineIndex = spineIndex_;
  meta.layoutKey = key;
  meta.sourceUncompressedSize = sourceUncompressedSize;
  meta.generationId = generationId;
  if (!writeMeta(meta)) {
    removeAllFiles();
    return false;
  }
  return true;
}

bool Cache::appendPage(const uint32_t pageNumber, const Page& page, const uint16_t paragraphIndex,
                       const uint16_t listItemIndex, const uint32_t sourceByteOffset) {
  if (pageNumber != knownPageCount()) {
    LOG_ERR("ISC", "Unexpected page append order: got %lu expected %lu", static_cast<unsigned long>(pageNumber),
            static_cast<unsigned long>(knownPageCount()));
    return false;
  }

  FsFile pages = Storage.open(paths_.pages.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!pages) {
    return false;
  }
  const uint32_t offset = pages.size();
  pages.seek(offset);
  if (!page.serialize(pages)) {
    pages.close();
    return false;
  }
  const uint32_t end = pages.position();
  pages.close();

  FsFile index = Storage.open(paths_.index.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!index) {
    return false;
  }
  PageIndexRecord record;
  record.pageOffset = offset;
  record.pageLength = end - offset;
  record.paragraphIndex = paragraphIndex;
  record.listItemIndex = listItemIndex;
  record.sourceByteOffset = sourceByteOffset;
  if (!checkedWriteIndexRecord(index, record)) {
    index.close();
    return false;
  }
  index.close();
  return true;
}

bool Cache::appendAnchor(const std::string& anchor, const uint16_t page) {
  FsFile file = Storage.open(paths_.anchors.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    return false;
  }
  if (!checkedWriteString(file, anchor, "anchor.key") || !checkedWritePod(file, page, "anchor.page")) {
    file.close();
    return false;
  }
  file.close();
  return true;
}

bool Cache::markComplete(const uint32_t finalPageCount, const uint32_t anchorCount) {
  Meta meta;
  if (!loadMeta(meta)) {
    return false;
  }
  if (knownPageCount() != finalPageCount) {
    LOG_ERR("ISC", "Final page count mismatch: known=%lu final=%lu", static_cast<unsigned long>(knownPageCount()),
            static_cast<unsigned long>(finalPageCount));
    return false;
  }
  meta.state = CacheState::Complete;
  meta.finalPageCount = finalPageCount;
  meta.anchorCount = anchorCount;
  return writeMeta(meta);
}

bool Cache::readIndexRecord(const uint32_t pageNumber, PageIndexRecord& out) const {
  FsFile index;
  if (!Storage.openFileForRead("ISC", paths_.index, index)) {
    return false;
  }
  const size_t indexSize = index.size();
  const size_t recordCount = indexSize / PAGE_INDEX_RECORD_SIZE;
  if (pageNumber >= recordCount) {
    index.close();
    return false;
  }
  if (!index.seek(pageNumber * PAGE_INDEX_RECORD_SIZE)) {
    index.close();
    return false;
  }
  readIndexRecordFromFile(index, out);
  index.close();
  return true;
}

std::unique_ptr<Page> Cache::loadPage(const uint32_t pageNumber) const {
  PageIndexRecord record;
  if (!readIndexRecord(pageNumber, record)) {
    return nullptr;
  }
  FsFile pages;
  if (!Storage.openFileForRead("ISC", paths_.pages, pages)) {
    return nullptr;
  }
  if (!pages.seek(record.pageOffset)) {
    pages.close();
    return nullptr;
  }
  auto page = Page::deserialize(pages);
  pages.close();
  return page;
}

std::optional<uint16_t> Cache::getPageForAnchor(const std::string& anchor) const {
  Meta meta;
  if (!loadMeta(meta) || meta.anchorCount == 0) {
    return std::nullopt;
  }
  FsFile file;
  if (!Storage.openFileForRead("ISC", paths_.anchors, file)) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < meta.anchorCount && file.available() > 0; i++) {
    std::string key;
    uint16_t page = 0;
    if (!readAnchorEntry(file, key, page)) {
      file.close();
      return std::nullopt;
    }
    if (key == anchor) {
      file.close();
      return page;
    }
  }
  file.close();
  return std::nullopt;
}

std::optional<uint16_t> Cache::getParagraphIndexForPage(const uint32_t pageNumber) const {
  PageIndexRecord record;
  if (!readIndexRecord(pageNumber, record)) {
    return std::nullopt;
  }
  return record.paragraphIndex;
}

std::optional<uint16_t> Cache::getPageForParagraphIndex(const uint16_t paragraphIndex) const {
  const uint32_t count = knownPageCount();
  if (count == 0) {
    return std::nullopt;
  }
  uint16_t resultPage = static_cast<uint16_t>(count - 1);
  FsFile index;
  if (!Storage.openFileForRead("ISC", paths_.index, index)) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < count; i++) {
    PageIndexRecord record;
    if (!readNextIndexRecord(index, record)) {
      index.close();
      return std::nullopt;
    }
    if (record.paragraphIndex >= paragraphIndex) {
      resultPage = static_cast<uint16_t>(i);
      break;
    }
  }
  index.close();
  return resultPage;
}

std::optional<uint16_t> Cache::getPageForListItemIndex(const uint16_t listItemIndex) const {
  const uint32_t count = knownPageCount();
  if (count == 0) {
    return std::nullopt;
  }
  uint16_t resultPage = static_cast<uint16_t>(count - 1);
  FsFile index;
  if (!Storage.openFileForRead("ISC", paths_.index, index)) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < count; i++) {
    PageIndexRecord record;
    if (!readNextIndexRecord(index, record)) {
      index.close();
      return std::nullopt;
    }
    if (record.listItemIndex >= listItemIndex) {
      resultPage = static_cast<uint16_t>(i);
      break;
    }
  }
  index.close();
  return resultPage;
}

uint32_t Cache::knownPageCount() const {
  if (!Storage.exists(paths_.index.c_str())) {
    return 0;
  }
  FsFile index;
  if (!Storage.openFileForRead("ISC", paths_.index, index)) {
    return 0;
  }
  const auto pageCount = knownPageCountFromIndexBytes(index.size());
  index.close();
  return pageCount;
}

bool Cache::isComplete() const {
  Meta meta;
  return loadMeta(meta) && meta.state == CacheState::Complete && meta.finalPageCount == knownPageCount();
}

bool Cache::isCompatibleWith(const LayoutCacheKey& key) const {
  Meta meta;
  return loadMeta(meta) && meta.layoutKey.matches(key);
}

bool Cache::hasPage(const uint32_t pageNumber) const { return pageNumber < knownPageCount(); }

void Cache::removeAllFiles() const {
  if (Storage.exists(paths_.meta.c_str())) {
    Storage.remove(paths_.meta.c_str());
  }
  if (Storage.exists(paths_.pages.c_str())) {
    Storage.remove(paths_.pages.c_str());
  }
  if (Storage.exists(paths_.index.c_str())) {
    Storage.remove(paths_.index.c_str());
  }
  if (Storage.exists(paths_.anchors.c_str())) {
    Storage.remove(paths_.anchors.c_str());
  }
}

}  // namespace IncrementalSection
