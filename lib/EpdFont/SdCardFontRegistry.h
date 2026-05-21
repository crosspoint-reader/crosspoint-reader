#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/.crosspoint/fonts/<Family>/<Family>_<size>.cpfont"
                      // e.g. "/.crosspoint/fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // kept for potential future formats
};

struct SdCardFontFamilyInfo {
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;

  // Pick the file whose pointSize is closest to targetPtSize. On ties (equal
  // distance) prefers the smaller pointSize so behaviour is deterministic
  // across SD card layouts. Returns nullptr when files is empty.
  const SdCardFontFileInfo* pickClosestSize(uint8_t targetPtSize) const;
};

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Primary (writable) location. All installs/downloads/deletes target this root.
  static constexpr const char* FONTS_DIR = "/.crosspoint/fonts";
  // Read-only fallback roots for upstream crosspoint-reader compatibility.
  // Upstream uses "/.fonts" (preferred, hidden) and "/fonts" (visible). Both are
  // scanned after the primary; primary wins on family-name conflicts.
  static constexpr const char* FONTS_DIR_UPSTREAM_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_UPSTREAM_VISIBLE = "/fonts";

  // Scan SD card, populate families_. Returns true if any families found.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  void scanRoot(const char* rootPath);
};
