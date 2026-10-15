#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/.crosspoint/fonts/<Family>/<Family>_<size>.cpfont"
                      // e.g. "/.crosspoint/fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14; 0 for a TTF file, which serves every size
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // a TTF file's head.macStyle: EpdFontFamily::Style
  // Byte count, read from the directory entry during discovery. Free here and
  // it saves an open() everywhere a caller only needs to tell whether the file
  // changed -- the font preview cache keys on it.
  uint32_t fileBytes = 0;
};

struct SdCardFontFamilyInfo {
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
  // A TrueType family: one .ttf file per style, regular among them, every size drawn from them on the
  // device (TtfFont). A directory with any .cpfont file is a .cpfont family, whatever else it holds.
  // Derived, not stored: a TTF file is listed with pointSize 0, which no .cpfont name can give, and a field
  // here would cost the registry's 128 reserved records heap for good.
  bool ttf() const { return !files.empty() && files.front().pointSize == 0; }

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;

  // Pick the file whose pointSize is closest to targetPtSize. On ties (equal
  // distance) prefers the smaller pointSize so behaviour is deterministic
  // across SD card layouts. Returns nullptr when files is empty, and for a TTF
  // family, which has no per-size files (callers that need one skip it).
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

  // Asked at discovery whether a TTF family with these style file sizes can be installed; one that cannot is not
  // listed, since picking it would only fall back to a built-in font. Unset (host tests): every family is listed.
  using TtfFits = bool (*)(const uint32_t* styleBytes, uint8_t count);
  void setTtfFits(const TtfFits fits) { ttfFits_ = fits; }

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  void addTtfFiles(const char* dirPath, const std::vector<std::string>& names, const std::vector<uint32_t>& bytes,
                   SdCardFontFamilyInfo& family) const;
  TtfFits ttfFits_ = nullptr;
  void scanRoot(const char* rootPath);
};
