#pragma once
#include <cstddef>
#include <cstdint>

/**
 * BookFusion reading position (EPUB).
 *
 * percentage: 0–100 (note: BookFusion uses 0-100, unlike KOReader's 0.0-1.0)
 * chapterIndex: spine index, 0-based
 * pagePositionInBook: (chapterIndex + fractional_position_in_chapter) / total_spine_count
 */
struct BookFusionPosition {
  float percentage = 0.0f;          // 0–100
  float pagePositionInBook = 0.0f;  // fractional book position
  int chapterIndex = 0;             // spine index, 0-based
};

/**
 * Response from the device-code auth endpoint.
 */
struct BookFusionDeviceCodeResponse {
  char deviceCode[256] = {};
  char userCode[16] = {};
  char verificationUri[128] = {};
  int interval = 5;     // seconds between polls
  int expiresIn = 600;  // seconds until code expires
};

/**
 * A single book from the user's BookFusion library.
 */
struct BookFusionBook {
  uint32_t id = 0;
  char title[64] = {};
  char authors[48] = {};
  char format[8] = {};  // "EPUB", "PDF", etc.
};

/**
 * Paginated result from the book search endpoint.
 * Request one extra item (MAX_BOOKS + 1) to detect whether more pages exist.
 */
struct BookFusionSearchResult {
  static constexpr int MAX_BOOKS = 20;
  BookFusionBook books[MAX_BOOKS];
  int count = 0;
  int currentPage = 0;
  bool hasMore = false;
};

/**
 * HTTP client for the BookFusion API.
 *
 * Base URL: https://www.bookfusion.com
 * All authenticated requests use: Authorization: Bearer <token>
 *                                 Accept: application/json; api_version=10
 *
 * Authentication uses the OAuth 2.0 Device Code flow:
 *   1. requestDeviceCode() → display verificationUri + userCode to user
 *   2. pollForToken() every interval seconds → returns OK + token when authorised
 *
 * Progress API:
 *   getProgress(bookId, out)  → GET /api/user/books/{id}/reading_position
 *   setProgress(bookId, pos)  → POST /api/user/books/{id}/reading_position
 */
class BookFusionSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_TOKEN,       // BF_TOKEN_STORE has no token
    NETWORK_ERROR,  // HTTP/TLS failure
    AUTH_FAILED,    // 401 Unauthorized
    SERVER_ERROR,   // 5xx or unexpected code
    JSON_ERROR,     // Failed to parse response
    NOT_FOUND,      // 404 — no progress exists yet
    PENDING,        // authorization_pending — keep polling
    SLOW_DOWN,      // slow_down — increase poll interval
    EXPIRED,        // expired_token
    DENIED,         // access_denied by user
  };

  // --- Device Code Auth (unauthenticated) ---
  static Error requestDeviceCode(BookFusionDeviceCodeResponse& out);
  static Error pollForToken(const char* deviceCode, char* outToken, size_t tokenMaxLen);

  // --- Progress ---
  static Error getProgress(uint32_t bookId, BookFusionPosition& out);
  static Error setProgress(uint32_t bookId, const BookFusionPosition& pos);

  // --- Library Browse & Download ---
  static Error searchBooks(int page, BookFusionSearchResult& out, const char* list = nullptr,
                           const char* sort = nullptr);
  static Error getDownloadUrl(uint32_t bookId, char* outUrl, size_t maxLen);

  static const char* errorString(Error error);

  static constexpr char API_ACCEPT[] = "application/json; api_version=10";

 private:
  static constexpr char BASE_URL[] = "https://www.bookfusion.com";
  static constexpr char CLIENT_ID[] = "koreader";
  static constexpr char DEVICE_CODE_GRANT_TYPE[] = "urn:ietf:params:oauth:grant-type:device_code";
};
