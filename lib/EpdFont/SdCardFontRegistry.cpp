#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

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

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // Expected: <anything>_<size>_<style>.cpfont
  // Find .cpfont extension
  const char* ext = strstr(filename, ".cpfont");
  if (!ext) return false;

  // Work backward from extension to find style and size segments
  // Copy the base name (without extension) for parsing
  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  // Find last underscore -> style
  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* styleStr = lastUnderscore + 1;
  *lastUnderscore = '\0';

  // Find second-to-last underscore -> size
  char* sizeUnderscore = strrchr(base, '_');
  if (!sizeUnderscore) return false;

  const char* sizeStr = sizeUnderscore + 1;

  // Parse size
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);

  // Parse style
  if (strcmp(styleStr, "regular") == 0)
    style = 0;
  else if (strcmp(styleStr, "bold") == 0)
    style = 1;
  else if (strcmp(styleStr, "italic") == 0)
    style = 2;
  else if (strcmp(styleStr, "bolditalic") == 0)
    style = 3;
  else
    return false;

  return true;
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) continue;

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = size;
    info.style = style;
    family.files.push_back(std::move(info));
  }
  dir.close();
}

bool SdCardFontRegistry::discover() {
  families_.clear();

  FsFile root = Storage.open(FONTS_DIR);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", FONTS_DIR);
    return false;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", FONTS_DIR);
    root.close();
    return false;
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

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(FONTS_DIR) + "/" + nameBuffer;
      scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
        families_.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files)", families_.back().name.c_str(),
                static_cast<int>(families_.back().files.size()));
      }
    } else {
      // Backward compat: loose .cpfont files in the fonts dir
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip macOS resource fork files (._*) and other hidden files
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      uint8_t size, style;
      if (!parseFilename(nameBuffer, size, style)) continue;

      // Derive family name from filename prefix (everything before _<size>_<style>.cpfont)
      char base[128];
      strncpy(base, nameBuffer, sizeof(base) - 1);
      base[sizeof(base) - 1] = '\0';
      // Remove _<size>_<style>.cpfont suffix to get the family name
      char* lastU = strrchr(base, '_');
      if (lastU) *lastU = '\0';
      lastU = strrchr(base, '_');
      if (lastU) *lastU = '\0';
      std::string familyName = base;

      // Find or create the family
      SdCardFontFamilyInfo* existing = nullptr;
      for (auto& f : families_) {
        if (f.name == familyName) {
          existing = &f;
          break;
        }
      }
      if (!existing) {
        families_.push_back(SdCardFontFamilyInfo{familyName, {}});
        existing = &families_.back();
      }

      SdCardFontFileInfo info;
      info.path = std::string(FONTS_DIR) + "/" + nameBuffer;
      info.pointSize = size;
      info.style = style;
      existing->files.push_back(std::move(info));
    }

    if (static_cast<int>(families_.size()) >= MAX_SD_FAMILIES) break;
  }
  root.close();

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
