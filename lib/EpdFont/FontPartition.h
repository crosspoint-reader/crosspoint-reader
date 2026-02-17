#pragma once

#include "EpdFont.h"
#include "EpdFontData.h"

/// Loads font data from the "fontdata" flash partition via esp_partition_mmap.
///
/// Fonts are organized into groups. The "ui" group is mapped once at boot and
/// stays resident. Reader font groups (e.g. "bookerly", "notosans",
/// "opendyslexic") are mapped on demand — only one reader group is active at
/// a time, and switching groups unmaps the previous one.
class FontPartition {
 public:
  /// Read partition header and directories, then mmap the "ui" font group.
  /// Must be called once before any other method.
  static bool begin();

  /// Map a reader font group by name. Unmaps the previous reader group if one
  /// was loaded. No-op if the requested group is already active.
  /// Returns false if the group is not found.
  static bool loadReaderGroup(const char* groupName);

  /// Returns the name of the currently loaded reader group, or nullptr.
  static const char* currentReaderGroup();

  /// Look up a font by name across both mapped groups.
  /// Returns nullptr if the font is not found or its group is not mapped.
  static const EpdFontData* getFont(const char* name);

  /// Get a stable EpdFont pointer by name. The returned pointer is valid for
  /// the lifetime of the program. Its underlying data automatically reflects
  /// the current mmap state (zeroed when the font's group is unmapped).
  /// Returns nullptr if the font name is not found in the partition.
  static EpdFont* getEpdFont(const char* name);

  /// Total number of fonts in the partition (across all groups).
  static int fontCount();

 private:
  FontPartition() = delete;
};
