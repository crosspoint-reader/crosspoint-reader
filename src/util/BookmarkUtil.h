#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class BookmarkUtil {
 public:
  static std::string getBookmarksDir();
  static std::string getBookmarkPath(const std::string& bookPath);
  static std::string sanitizeBookmarkSummary(std::string summary);

  // CrossPoint Sync bookmark id: the first 8 bytes of SHA-256(xpath). Every
  // device must derive the same id, so the wire form is 16 lowercase hex digits.
  static constexpr size_t SYNC_ID_HEX_SIZE = 17;
  static uint64_t syncId(const std::string& xpath);
  static void formatSyncId(uint64_t id, char* out);  // out holds SYNC_ID_HEX_SIZE bytes
  static bool parseSyncId(const char* hex, uint64_t& id);
};
