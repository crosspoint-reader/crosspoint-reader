#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>

/// Shared utility for font installation (device download + browser upload).
/// Handles directory creation, file validation, deletion, and registry refresh.
class FontInstaller {
 public:
  enum class Error {
    OK,
    INVALID_FAMILY_NAME,
    INVALID_FILE,
    SD_WRITE_ERROR,
    MAX_FAMILIES_REACHED,
  };

  explicit FontInstaller(SdCardFontRegistry& registry);

  /// Validate a family name: alphanumeric + hyphen + underscore only, no path traversal.
  static bool isValidFamilyName(const char* name);

  /// Ensure /.crosspoint/fonts/<family>/ directory exists.
  bool ensureFamilyDir(const char* familyName);

  /// Validate a .cpfont file on disk (check magic bytes).
  bool validateCpfontFile(const char* path);

  /// Build the full SD path for a font file.
  /// Writes "/.crosspoint/fonts/<family>/<filename>" to outBuf.
  static void buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize);

  /// Delete a family directory and all .cpfont files in it.
  /// If the deleted family is the active reader font, clears the setting.
  Error deleteFamily(const char* familyName);

  /// Re-run registry discovery to pick up new/removed fonts.
  void refreshRegistry();

  /// Check whether a family name already exists in the registry.
  bool isFamilyInstalled(const char* familyName) const;

 private:
  SdCardFontRegistry& registry_;

  static constexpr const char* CPFONT_MAGIC = "CPFONT\0";
  static constexpr size_t CPFONT_MAGIC_LEN = 8;
};
