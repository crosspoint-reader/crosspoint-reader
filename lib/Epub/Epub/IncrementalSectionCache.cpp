#include "IncrementalSectionCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <common/FsApiConstants.h>

#include <utility>

#include "Page.h"

namespace IncrementalSection {
namespace {

bool ensureDirectory(const std::string& path) {
  if (Storage.exists(path.c_str())) {
    return true;
  }
  return Storage.mkdir(path.c_str());
}

void writeLayoutKey(FsFile& file, const LayoutCacheKey& key) {
  serialization::writePod(file, key.cacheVersion);
  serialization::writePod(file, key.fontId);
  serialization::writePod(file, key.lineCompression);
  serialization::writePod(file, key.extraParagraphSpacing);
  serialization::writePod(file, key.paragraphAlignment);
  serialization::writePod(file, key.viewportWidth);
  serialization::writePod(file, key.viewportHeight);
  serialization::writePod(file, key.hyphenationEnabled);
  serialization::writePod(file, key.embeddedStyle);
  serialization::writePod(file, key.imageRendering);
  serialization::writePod(file, key.focusReadingEnabled);
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

void writeIndexRecord(FsFile& file, const PageIndexRecord& record) {
  serialization::writePod(file, record.pageOffset);
  serialization::writePod(file, record.pageLength);
  serialization::writePod(file, record.paragraphIndex);
  serialization::writePod(file, record.listItemIndex);
  serialization::writePod(file, record.sourceByteOffset);
}

void readIndexRecordFromFile(FsFile& file, PageIndexRecord& record) {
  serialization::readPod(file, record.pageOffset);
  serialization::readPod(file, record.pageLength);
  serialization::readPod(file, record.paragraphIndex);
  serialization::readPod(file, record.listItemIndex);
  serialization::readPod(file, record.sourceByteOffset);
}

}  // namespace

Cache::Cache(std::string sectionsDir, const uint32_t spineIndex)
    : sectionsDir_(std::move(sectionsDir)), spineIndex_(spineIndex), paths_(pathsForSection(sectionsDir_, spineIndex)) {}

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

  serialization::writePod(file, CACHE_MAGIC);
  serialization::writePod(file, CACHE_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(meta.state));
  serialization::writePod(file, meta.spineIndex);
  writeLayoutKey(file, meta.layoutKey);
  serialization::writePod(file, meta.sourceUncompressedSize);
  serialization::writePod(file, meta.finalPageCount);
  serialization::writePod(file, meta.anchorCount);
  serialization::writePod(file, meta.generationId);
  file.close();
  return true;
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
  writeIndexRecord(index, record);
  index.close();
  return true;
}

bool Cache::appendAnchor(const std::string& anchor, const uint16_t page) {
  FsFile file = Storage.open(paths_.anchors.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    return false;
  }
  serialization::writeString(file, anchor);
  serialization::writePod(file, page);
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
  if ((pageNumber + 1) * PAGE_INDEX_RECORD_SIZE > indexSize) {
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
    serialization::readString(file, key);
    serialization::readPod(file, page);
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
  for (uint32_t i = 0; i < count; i++) {
    PageIndexRecord record;
    if (!readIndexRecord(i, record)) {
      return std::nullopt;
    }
    if (record.paragraphIndex >= paragraphIndex) {
      resultPage = static_cast<uint16_t>(i);
      break;
    }
  }
  return resultPage;
}

std::optional<uint16_t> Cache::getPageForListItemIndex(const uint16_t listItemIndex) const {
  const uint32_t count = knownPageCount();
  if (count == 0) {
    return std::nullopt;
  }
  uint16_t resultPage = static_cast<uint16_t>(count - 1);
  for (uint32_t i = 0; i < count; i++) {
    PageIndexRecord record;
    if (!readIndexRecord(i, record)) {
      return std::nullopt;
    }
    if (record.listItemIndex >= listItemIndex) {
      resultPage = static_cast<uint16_t>(i);
      break;
    }
  }
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
