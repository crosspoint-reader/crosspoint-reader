#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class RecentBooksStore;
class OpdsServerStore;
struct BookmarkEntry;

namespace JsonSettingsIO {

constexpr size_t BOOKMARK_FILE_MAX_BYTES = 50U * 1024U;

enum class BookmarkLoadStatus : uint8_t { Loaded, Missing, Invalid, Oversize, IoError };

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

// Bookmarks
bool saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path);
bool loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json);
BookmarkLoadStatus loadBookmarksFromFile(std::vector<BookmarkEntry>& bookmarks, const char* path);

}  // namespace JsonSettingsIO
