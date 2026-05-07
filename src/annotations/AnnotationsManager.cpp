#include "AnnotationsManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>

#include <algorithm>

static constexpr const char* ANNOT_FILENAME = "/annotations.bin";

std::string AnnotationsManager::annotationsPath(const char* bookCachePath) {
  return std::string(bookCachePath) + ANNOT_FILENAME;
}

static bool readString(HalFile& file, std::string& out) {
  uint16_t len = 0;
  if (file.read(&len, sizeof(len)) != sizeof(len)) return false;
  if (len == 0) {
    out.clear();
    return true;
  }
  out.resize(len);
  return file.read(&out[0], len) == len;
}

static void writeString(HalFile& file, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(s.size());
  file.write(&len, sizeof(len));
  if (len > 0) {
    file.write(s.c_str(), len);
  }
}

bool AnnotationsManager::load(const char* bookCachePath) {
  records.clear();
  const std::string path = annotationsPath(bookCachePath);

  HalFile file;
  if (!Storage.openFileForRead("ANNOT", path.c_str(), file)) {
    return true;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (file.read(&version, 1) != 1 || version < 4 || version > FILE_VERSION) {
    file.close();
    return true;
  }
  if (file.read(&count, sizeof(count)) != sizeof(count)) {
    file.close();
    return false;
  }

  records.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    AnnotationRecord rec;
    if (file.read(&rec.sectionIdx, sizeof(rec.sectionIdx)) != sizeof(rec.sectionIdx)) break;
    if (file.read(&rec.sectionPage, sizeof(rec.sectionPage)) != sizeof(rec.sectionPage)) break;
    if (version >= 5) {
      if (file.read(&rec.endSectionPage, sizeof(rec.endSectionPage)) != sizeof(rec.endSectionPage)) break;
    } else {
      rec.endSectionPage = rec.sectionPage;
    }
    if (version >= 6) {
      if (file.read(&rec.wordCount, sizeof(rec.wordCount)) != sizeof(rec.wordCount)) break;
    } else {
      rec.wordCount = 0;
    }
    if (!readString(file, rec.startText)) break;
    if (!readString(file, rec.endText)) break;
    if (version >= 6) {
      if (!readString(file, rec.beforeStartText)) break;
      if (!readString(file, rec.afterEndText)) break;
    }
    if (version >= 7) {
      if (!readString(file, rec.midText)) break;
    }
    records.push_back(std::move(rec));
  }

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
    file.write(&rec.sectionPage, sizeof(rec.sectionPage));
    file.write(&rec.endSectionPage, sizeof(rec.endSectionPage));
    file.write(&rec.wordCount, sizeof(rec.wordCount));
    writeString(file, rec.startText);
    writeString(file, rec.endText);
    writeString(file, rec.beforeStartText);
    writeString(file, rec.afterEndText);
    writeString(file, rec.midText);
  }

  file.flush();
  file.close();
  LOG_DBG("ANNOT", "Saved %zu annotations to %s", records.size(), path.c_str());
  return true;
}

void AnnotationsManager::add(AnnotationRecord record) { records.push_back(std::move(record)); }

std::vector<AnnotationsManager::AnnotationRecord> AnnotationsManager::forSection(uint16_t sectionIdx) const {
  std::vector<AnnotationRecord> result;
  std::copy_if(records.begin(), records.end(), std::back_inserter(result),
               [sectionIdx](const AnnotationRecord& rec) { return rec.sectionIdx == sectionIdx; });
  return result;
}

bool AnnotationsManager::hasAnnotationsForSection(uint16_t sectionIdx) const {
  return std::any_of(records.begin(), records.end(),
                     [sectionIdx](const AnnotationRecord& rec) { return rec.sectionIdx == sectionIdx; });
}
