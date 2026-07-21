#pragma once
#include <string>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  // Collision-resistant key used for all new writes.
  static std::string getBookmarkPath(const std::string& bookPath);
  // Pre-CrossVi flattened key. Read-only fallback: it is not injective and
  // must never be deleted as though ownership were proven.
  static std::string getLegacyBookmarkPath(const std::string& bookPath);
  // Exact empty bookmark files are used as canonical tombstones. They shadow
  // the ambiguous legacy fallback without deleting a file that another path
  // may still own.
  static bool isEmptyBookmarkFile(const std::string& path);
  static bool writeEmptyBookmarkFile(const std::string& path);
  static bool writeEmptyCanonicalBookmark(const std::string& bookPath);
  // Archives the collision-resistant canonical file inside the old book cache,
  // then publishes a verified empty tombstone via temp+rename. Safe to retry
  // after any rename boundary; the ambiguous legacy fallback is never moved.
  static bool quarantineCanonicalForReplacement(const std::string& bookPath, const std::string& cachePath);
  // Keep an existing legacy fallback from resurfacing after a move. Any old
  // canonical at this path is deliberately replaced with verified empty data.
  static bool ensureLegacyBookmarkShadowed(const std::string& bookPath);
  static std::string sanitizeBookmarkSummary(std::string summary);
};
