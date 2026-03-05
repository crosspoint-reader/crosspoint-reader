#pragma once

#include <string>
#include <vector>

/**
 * A language entry in the runtime language list.
 * Covers core (flash-baked), installed non-core (SD YAML), and
 * available-for-download (manifest) languages.
 */
struct LanguageEntry {
  char code[8];      // e.g. "FI"
  char name[48];     // native display name, e.g. "Suomi"
  bool isCore;       // true = baked into flash
  bool isInstalled;  // true = YAML present on SD (always true for core)
};

/**
 * Builds and manages the runtime language list.
 *
 * Call buildList() on each LanguageSelectActivity enter.
 * The list is: core languages first (EN, then core alpha), then non-core
 * languages sorted alphabetically by name (installed and downloadable mixed,
 * ordered so installed ones sort with the same name).
 */
class LanguageRegistry {
 public:
  // Build the full list: core + SD-installed + manifest.
  // EN is always first; remaining entries sorted alphabetically by name.
  static std::vector<LanguageEntry> buildList();

  // True if /.crosspoint/languages/manifest.json exists on SD.
  static bool hasManifest();
};
