#include "AnnotationsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

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
  }

  file.flush();
  file.close();
  LOG_DBG("ANNOT", "Saved %zu annotations to %s", records.size(), path.c_str());
  return true;
}

void AnnotationsManager::add(AnnotationRecord record) { records.push_back(std::move(record)); }

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
