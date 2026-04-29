#include "AnnotationsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include <algorithm>
#include <cstring>

static constexpr const char* ANNOT_FILENAME = "/annotations.bin";

std::string AnnotationsManager::annotationsPath(const char* bookCachePath) {
  return std::string(bookCachePath) + ANNOT_FILENAME;
}

bool AnnotationsManager::load(const char* bookCachePath) {
  records.clear();
  const std::string path = annotationsPath(bookCachePath);

  HalFile file;
  if (!Storage.openFileForRead("ANNOT", path.c_str(), file)) {
    return true;  // No annotations file yet — not an error
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (file.read(&version, 1) != 1 || version != FILE_VERSION) {
    file.close();
    return true;  // Unknown version — treat as empty
  }
  if (file.read(&count, sizeof(count)) != sizeof(count)) {
    file.close();
    return false;
  }

  records.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    AnnotationRecord rec;
    if (file.read(&rec.sectionIdx, sizeof(rec.sectionIdx)) != sizeof(rec.sectionIdx)) break;

    uint16_t rectCount = 0;
    if (file.read(&rectCount, sizeof(rectCount)) != sizeof(rectCount)) break;
    rec.rects.resize(rectCount);
    for (uint16_t r = 0; r < rectCount; ++r) {
      if (file.read(&rec.rects[r], sizeof(Rect)) != sizeof(Rect)) goto done;
    }

    uint8_t previewLen = 0;
    if (file.read(&previewLen, 1) != 1) break;
    if (previewLen > 0) {
      rec.textPreview.resize(previewLen);
      if (file.read(rec.textPreview.data(), previewLen) != previewLen) break;
    }

    records.push_back(std::move(rec));
  }

done:
  file.close();
  LOG_DBG("ANNOT", "Loaded %zu annotations from %s", records.size(), path.c_str());
  return true;
}

bool AnnotationsManager::save(const char* bookCachePath) const {
  const std::string path = annotationsPath(bookCachePath);

  HalFile file = Storage.open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("ANNOT", "Failed to open %s for write", path.c_str());
    return false;
  }

  const uint8_t version = FILE_VERSION;
  const uint16_t count = static_cast<uint16_t>(records.size());
  file.write(&version, 1);
  file.write(&count, sizeof(count));

  for (const auto& rec : records) {
    file.write(&rec.sectionIdx, sizeof(rec.sectionIdx));
    const uint16_t rectCount = static_cast<uint16_t>(rec.rects.size());
    file.write(&rectCount, sizeof(rectCount));
    for (const auto& r : rec.rects) {
      file.write(&r, sizeof(Rect));
    }
    const uint8_t previewLen = static_cast<uint8_t>(
        rec.textPreview.size() < MAX_PREVIEW_LEN ? rec.textPreview.size() : MAX_PREVIEW_LEN);
    file.write(&previewLen, 1);
    if (previewLen > 0) {
      file.write(rec.textPreview.c_str(), previewLen);
    }
  }

  file.flush();
  file.close();
  LOG_DBG("ANNOT", "Saved %zu annotations to %s", records.size(), path.c_str());
  return true;
}

void AnnotationsManager::add(AnnotationRecord record) {
  records.push_back(std::move(record));
}

void AnnotationsManager::removeMeta(size_t idx) {
  if (idx < records.size()) {
    records.erase(records.begin() + static_cast<ptrdiff_t>(idx));
  }
}

bool AnnotationsManager::permanentDelete(size_t idx, const char* clippingFilePath) {
  if (idx >= records.size()) return false;
  const std::string preview = records[idx].textPreview;

  // Streaming copy of clipping file, skipping the matching entry
  // Each entry in the Kindle format ends with "\n==========\n"
  static constexpr char SEPARATOR[] = "\n==========\n";
  static constexpr size_t SEP_LEN = sizeof(SEPARATOR) - 1;

  // Build temp path alongside the source file
  const std::string tempPath = std::string(clippingFilePath) + ".tmp";

  HalFile src = Storage.open(clippingFilePath, O_RDONLY);
  if (!src) {
    LOG_ERR("ANNOT", "Failed to open %s for reading", clippingFilePath);
    removeMeta(idx);
    return false;
  }

  HalFile dst = Storage.open(tempPath.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!dst) {
    src.close();
    LOG_ERR("ANNOT", "Failed to open temp file %s", tempPath.c_str());
    return false;
  }

  // Read file entry-by-entry using separator as delimiter
  std::string entry;
  entry.reserve(STREAM_BLOCK);
  char buf[STREAM_BLOCK];
  bool skipped = false;

  // We accumulate until we find a separator, then decide to copy or skip
  while (true) {
    int32_t n = src.read(buf, sizeof(buf));
    if (n <= 0) break;
    entry.append(buf, static_cast<size_t>(n));

    // Process all complete entries in the accumulated buffer
    size_t pos = 0;
    while (true) {
      size_t sepPos = entry.find(SEPARATOR, pos);
      if (sepPos == std::string::npos) break;

      const std::string chunk = entry.substr(pos, sepPos + SEP_LEN - pos);

      // Check if this entry contains the preview text we want to delete
      if (!skipped && !preview.empty() && chunk.find(preview) != std::string::npos) {
        skipped = true;  // Skip this entry
      } else {
        dst.write(chunk.c_str(), chunk.size());
      }
      pos = sepPos + SEP_LEN;
    }
    entry = entry.substr(pos);  // Keep leftover (incomplete entry)
  }

  // Write any trailing content that doesn't end with a separator
  if (!entry.empty()) {
    dst.write(entry.c_str(), entry.size());
  }

  src.close();
  dst.flush();
  dst.close();

  // Replace original with temp
  Storage.remove(clippingFilePath);
  Storage.rename(tempPath.c_str(), clippingFilePath);

  removeMeta(idx);
  LOG_DBG("ANNOT", "Permanently deleted annotation %zu from %s (skipped=%d)", idx, clippingFilePath, skipped);
  return true;
}

std::vector<AnnotationsManager::AnnotationRecord> AnnotationsManager::forSection(uint16_t sectionIdx) const {
  std::vector<AnnotationRecord> result;
  for (const auto& rec : records) {
    if (rec.sectionIdx == sectionIdx) {
      result.push_back(rec);
    }
  }
  return result;
}

bool AnnotationsManager::hasAnnotationsForSection(uint16_t sectionIdx) const {
  for (const auto& rec : records) {
    if (rec.sectionIdx == sectionIdx) return true;
  }
  return false;
}

size_t AnnotationsManager::firstIndexForSection(uint16_t sectionIdx) const {
  for (size_t i = 0; i < records.size(); ++i) {
    if (records[i].sectionIdx == sectionIdx) return i;
  }
  return SIZE_MAX;
}
