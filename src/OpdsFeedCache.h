#pragma once

#include <OpdsParser.h>

#include <string>
#include <vector>

struct OpdsCachedEntry {
  OpdsEntry entry;        // title, author, href, id, type
  std::string localPath;  // "/opds-books/..." or empty if not yet downloaded
};

/**
 * Disk cache for a single OPDS server's root feed entries.
 * Persists the article/book list so the browser can show content without
 * a WiFi connection. Also records the local file path for each downloaded
 * book so matching is done by stable entry ID rather than guessed filename.
 *
 * Cache file: /.crosspoint/opds_<4hex>.json
 */
class OpdsFeedCache {
 public:
  /**
   * Load cache for the given server URL.
   * Returns false (and leaves the cache empty) if no cache exists or the
   * stored URL does not match serverUrl.
   */
  bool load(const std::string& serverUrl);

  /**
   * Save (or overwrite) the cache for serverUrl with the provided entries.
   * Capped at MAX_CACHED_ENTRIES to bound RAM usage.
   */
  bool save(const std::string& serverUrl, const std::vector<OpdsEntry>& entries);

  /**
   * Update a single entry's local download path in memory and re-save.
   * No-op (returns false) if entryId is not found in the cache.
   */
  bool updateLocalPath(const std::string& entryId, const std::string& path);

  /** O(n) lookup by entry id. Returns empty string if not found. */
  std::string getLocalPath(const std::string& entryId) const;

  const std::vector<OpdsCachedEntry>& getEntries() const { return cachedEntries; }
  bool isLoaded() const { return !cachedServerUrl.empty(); }
  bool isEmpty() const { return cachedEntries.empty(); }
  void clear() {
    cachedEntries.clear();
    cachedServerUrl.clear();
  }

 private:
  static constexpr int CACHE_FORMAT_VERSION = 1;
  static constexpr size_t MAX_CACHED_ENTRIES = 100;

  std::vector<OpdsCachedEntry> cachedEntries;
  std::string cachedServerUrl;

  static std::string cachePath(const std::string& serverUrl);
  bool saveInternal() const;
};
