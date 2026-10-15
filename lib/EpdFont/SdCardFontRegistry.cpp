#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ttf/TtfFamily.h>

#include <algorithm>
#include <cstring>
#include <numeric>

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  if (ttf()) return sizes;  // drawn at any size, from one file per style
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::pickClosestSize(uint8_t targetPtSize) const {
  if (ttf()) return nullptr;
  const SdCardFontFileInfo* selected = nullptr;
  int bestDiff = INT32_MAX;
  for (const auto& f : files) {
    int diff = std::abs(static_cast<int>(f.pointSize) - static_cast<int>(targetPtSize));
    // Strict < ensures the first scan wins on ties; then tie-break by smaller
    // pointSize to make the choice independent of filesystem enumeration order.
    if (diff < bestDiff || (diff == bestDiff && selected && f.pointSize < selected->pointSize)) {
      bestDiff = diff;
      selected = &f;
    }
  }
  return selected;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  style = 0;
  return true;
}

// A directory of .ttf files: each file's style and weight come from its head and OS/2 tables, and the file of
// each style nearest the regular or bold weight serves it (ttf::assignStyles). Only a directory with a regular
// style becomes a family.
void SdCardFontRegistry::addTtfFiles(const char* dirPath, const std::vector<std::string>& names,
                                     const std::vector<uint32_t>& bytes, SdCardFontFamilyInfo& family) const {
  std::vector<size_t> order(names.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return names[a] < names[b]; });
  std::vector<ttf::FileProbe> probes(order.size());
  for (size_t i = 0; i < order.size(); ++i) {
    const std::string path = std::string(dirPath) + "/" + names[order[i]];
    HalFile file;
    if (!Storage.openFileForRead("SDREG", path.c_str(), file)) continue;
    probes[i] = ttf::probeFile(ttf::fileSource(file, bytes[order[i]]));
    if (probes[i].style < 0) {
      LOG_DBG("SDREG", "Not a TrueType font the reader draws: %s", path.c_str());
    }
  }
  int chosen[4];
  if (!ttf::assignStyles(probes.data(), probes.size(), chosen)) {
    LOG_DBG("SDREG", "No regular TrueType style in %s", dirPath);
    return;
  }
  uint32_t styleBytes[4];
  uint8_t styleCount = 0;
  for (const int file : chosen) {
    if (file >= 0) styleBytes[styleCount++] = bytes[order[static_cast<size_t>(file)]];
  }
  if (ttfFits_ && !ttfFits_(styleBytes, styleCount)) {
    LOG_INF("SDREG", "TTF family in %s is too large for the font partition; not listed", dirPath);
    return;
  }
  for (uint8_t style = 0; style < 4; ++style) {
    if (chosen[style] < 0) continue;
    const size_t file = order[static_cast<size_t>(chosen[style])];
    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + names[file];
    info.pointSize = 0;
    info.style = style;
    info.fileBytes = bytes[file];
    family.files.push_back(std::move(info));
  }
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  std::vector<std::string> ttfNames;
  std::vector<uint32_t> ttfBytes;
  char nameBuffer[128];
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    const auto entryBytes = static_cast<uint32_t>(entry.fileSize());
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) {
      if (ttf::isTtfFileName(nameBuffer)) {
        ttfNames.emplace_back(nameBuffer);
        ttfBytes.push_back(entryBytes);
      }
      continue;
    }

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = size;
    info.style = style;
    info.fileBytes = entryBytes;
    family.files.push_back(std::move(info));
  }
  dir.close();
  if (family.files.empty() && !ttfNames.empty()) addTtfFiles(dirPath, ttfNames, ttfBytes, family);
}

void SdCardFontRegistry::scanRoot(const char* rootPath) {
  FsFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    root.close();
    return;
  }

  char nameBuffer[128];
  while (true) {
    FsFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      // Subdirectory = font family
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      // Skip in-flight install/rollback dirs (Bookerly__staging, Bookerly__backup, ...)
      // — these may contain valid-looking .cpfont files during a resumable
      // download but must not be exposed as installable families.
      const size_t nameLen = strlen(nameBuffer);
      static constexpr const char* kStagingSuffix = "__staging";
      static constexpr const char* kBackupSuffix = "__backup";
      const size_t stagingLen = strlen(kStagingSuffix);
      const size_t backupLen = strlen(kBackupSuffix);
      if ((nameLen > stagingLen && strcmp(nameBuffer + nameLen - stagingLen, kStagingSuffix) == 0) ||
          (nameLen > backupLen && strcmp(nameBuffer + nameLen - backupLen, kBackupSuffix) == 0)) {
        continue;
      }

      // Primary wins: skip if the family was already discovered in an earlier root.
      bool alreadyKnown = false;
      for (const auto& f : families_) {
        if (f.name == nameBuffer) {
          alreadyKnown = true;
          break;
        }
      }
      if (alreadyKnown) {
        LOG_DBG("SDREG", "Skipping duplicate family %s in %s (primary wins)", nameBuffer, rootPath);
        continue;
      }

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
        families_.push_back(std::move(family));
        LOG_DBG("SDREG", "Found %s family: %s (%d files) in %s", families_.back().ttf() ? "TTF" : ".cpfont",
                families_.back().name.c_str(), static_cast<int>(families_.back().files.size()), rootPath);
      }
    } else {
      entry.close();
    }
  }
  root.close();
}

bool SdCardFontRegistry::discover() {
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Primary first so it wins on family-name conflicts; the upstream roots are
  // read-only fallbacks — installers/deleters always target only the primary.
  scanRoot(FONTS_DIR);
  scanRoot(FONTS_DIR_UPSTREAM_HIDDEN);
  scanRoot(FONTS_DIR_UPSTREAM_VISIBLE);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
