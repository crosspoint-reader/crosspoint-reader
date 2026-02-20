#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * A single article from the Wallabag API.
 */
struct WallabagArticle {
  int id = 0;
  std::string title;
  int64_t updatedAt = 0;  // Unix timestamp
};

/**
 * HTTP client for the Wallabag REST API.
 *
 * API endpoints used:
 *   POST /oauth/v2/token          — Obtain OAuth2 access token
 *   GET  /api/entries.json        — List unread articles
 *   GET  /api/entries/{id}/export.epub — Download article as EPUB
 *
 * Authentication: OAuth2 Bearer token stored in WallabagCredentialStore.
 * Token is automatically refreshed when expired.
 */
class WallabagClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
  };

  /**
   * Authenticate with the Wallabag server (obtain OAuth2 token).
   * Token is stored in WALLABAG_STORE on success.
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Fetch list of unread articles, newest-first.
   * Automatically re-authenticates if token is invalid.
   * @param out Output: list of articles
   * @param limit Maximum number of articles to fetch (0 = use a large default)
   * @return OK on success, error code on failure
   */
  static Error fetchArticles(std::vector<WallabagArticle>& out, int limit);

  /**
   * Download an article as an EPUB file.
   * @param id Article ID
   * @param destPath Destination path on SD card
   * @param progress Optional progress callback(downloaded, total)
   * @return OK on success, error code on failure
   */
  static Error downloadArticle(int id, const std::string& destPath,
                               std::function<void(size_t, size_t)> progress = nullptr);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);
};
