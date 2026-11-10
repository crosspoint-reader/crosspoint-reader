#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "VectorFontSupport.h"

struct SdCardFontFileInfo {
  // Filename, stored only when SdCardFontFamilyInfo::filePath() cannot rebuild
  // it: a .cpfont not named "<Family>_<size>.cpfont", or any vector file. Null
  // for the canonical name, so downloaded families keep no per-file heap strings
  // and the entry is two pointers wide (8 bytes on the ESP32-C3).
  std::unique_ptr<char[]> name;
  uint8_t pointSize;  // parsed from filename: 14 (0 for size-free vector fonts)
  uint8_t style;      // .cpfont: always 0 (all 4 styles bundled in one file).
                      // Vector family in a folder: the style ROLE of this file —
                      // 0=regular, 1=bold, 2=italic, 3=bold-italic (parsed from
                      // the filename). A loose vector file is always role 0.
};

struct SdCardFontFamilyInfo {
  // Buffer size for filePath(), matching the installer's own path buffers.
  // Discovery drops files whose path would not fit, so every registered file
  // can be opened through filePath() with a buffer of this size.
  static constexpr size_t PATH_BUFFER_SIZE = 160;

  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
  // SdCardFontRegistry::FONTS_DIR_HIDDEN or FONTS_DIR_VISIBLE: static storage,
  // never freed, so a pointer is enough.
  const char* root = nullptr;
  // true for TrueType/OpenType files (.ttf/.otf/.ttc) rendered at any size via
  // the FreeInkFont engine (see TtfEpdFont / SdCardFontSystem): `files` holds
  // one entry per style role, each with its filename in `name` and pointSize 0
  // (size-free). false = a directory of pre-rasterized .cpfont files.
  bool vector = false;
  bool loose = false;  // vector file directly under `root`, no family directory

  // Writes the file's SD path, e.g. "/.fonts/NotoSansCJK/NotoSansCJK_14.cpfont".
  // Returns false when `outSize` is too small; `out` is then truncated.
  bool filePath(const SdCardFontFileInfo& file, char* out, size_t outSize) const;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  // Installed file closest to `pointSize` (ties → smaller). nullptr when the
  // family ships nothing in `style`.
  const SdCardFontFileInfo* findNearestSize(uint8_t pointSize, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;
};

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Two top-level roots are scanned at discovery time. Hidden is preferred
  // when creating new installs; both are read from if present.
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";

  // Returns the existing root for `familyName` (the one that contains
  // /<root>/<familyName>/), or nullptr if the family is not installed in
  // either root. Used by writers to keep re-installs in their existing dir.
  static const char* findFamilyRoot(const char* familyName);

  // Returns the root path that should be used when creating a brand-new
  // family on disk (no prior install): the existing root if exactly one of
  // the two roots exists, otherwise the hidden root.
  static const char* defaultWriteRoot();

  // Scan SD card, populate families_. Invalidates pointers and references
  // obtained from getFamilies()/findFamily(). Returns true if any families found.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

#if CROSSPOINT_VECTOR_FONTS
  // FtFont::ReadFn over a HalFile* ctx (absolute-offset reads; count 0 is a
  // seek probe). Shared by face inspection here and streamed TTF sources
  // (SdCardFontSystem).
  static unsigned long halFileRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
#endif

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  // True when `filename` is exactly "<family>_<size>.cpfont", the name
  // filePath() rebuilds without a stored string.
  static bool isCanonicalCpfontName(const char* filename, const std::string& family, uint8_t size);
#if CROSSPOINT_VECTOR_FONTS
  // Match a loose vector font filename (.ttf/.otf/.ttc, case-insensitive) and
  // return the length of the base name (extension stripped) in `baseLen`.
  static bool parseVectorFontName(const char* filename, size_t& baseLen);
  // Style role (0=regular, 1=bold, 2=italic, 3=bold-italic) inferred from a
  // vector font's base name (case-insensitive "bold"/"italic"/"oblique" tokens).
  static uint8_t parseVectorStyle(const char* baseName, size_t baseLen);
  // Refine each vector file's style role from its real face metadata
  // (FtFont::inspectStream: OS/2 weight + italic flag), keeping the
  // filename-derived role when the face can't be read. Then dedup by role.
  static void refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files);
#endif
  static void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  // Scan one root (e.g. "/.fonts"), append families to `out`, dedup by name.
  static void scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out);
};
