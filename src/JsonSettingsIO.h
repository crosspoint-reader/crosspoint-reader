#pragma once

#include <vector>

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class RecentBooksStore;
class OpdsServerStore;
class ReadingStatsStore;
struct BookmarkEntry;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadState(CrossPointState& s, const char* json);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// ReadingStatsStore
bool saveReadingStats(const ReadingStatsStore& store, const char* path);
bool loadReadingStats(ReadingStatsStore& store, const char* json);

// OpdsServerStore
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave = nullptr);

// Bookmarks
bool saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path);
bool loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json);

}  // namespace JsonSettingsIO
