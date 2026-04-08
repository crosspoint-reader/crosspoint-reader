#pragma once

/**
 * HardcoverClient — GraphQL HTTP client for Hardcover.app API.
 *
 * API Endpoint: https://api.hardcover.app/v1/graphql
 *
 * Authentication:
 *   Authorization: Bearer <token>
 *
 * All methods are static (no instance state), matching the
 * KOReaderSyncClient pattern.  Network I/O should only be called
 * from a background FreeRTOS task — never from plugin hooks.
 *
 * Status IDs:
 *   1 = Want to Read
 *   2 = Currently Reading
 *   3 = Read (finished)
 *   5 = Did Not Finish
 */
class HardcoverClient {
 public:
  enum Error {
    OK = 0,
    NO_TOKEN,
    NETWORK_ERROR,
    AUTH_FAILED,
    RATE_LIMITED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
  };

  // Status IDs used by Hardcover API
  static constexpr int STATUS_WANT_TO_READ = 1;
  static constexpr int STATUS_CURRENTLY_READING = 2;
  static constexpr int STATUS_READ = 3;
  static constexpr int STATUS_DID_NOT_FINISH = 5;

  /**
   * Validate the stored bearer token by querying `me { id }`.
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Search for a book by ISBN or title.
   * @param query  ISBN string or book title
   * @param outBookId  Output: the Hardcover book ID (0 if not found)
   * @return OK on success, NOT_FOUND if no results, error code on failure
   */
  static Error searchBook(const char* query, int& outBookId);

  /**
   * Add a book to the user's library with a given status.
   * @param bookId  Hardcover book ID (from searchBook)
   * @param statusId  Status to set (use STATUS_* constants)
   * @param outUserBookId  Output: the user_book relationship ID
   * @return OK on success, error code on failure
   */
  static Error addBook(int bookId, int statusId, int& outUserBookId);

  /**
   * Update the status of an existing user book entry.
   * @param userBookId  The user_book relationship ID
   * @param statusId  New status (use STATUS_* constants)
   * @return OK on success, error code on failure
   */
  static Error updateStatus(int userBookId, int statusId);

  /**
   * Post a reading progress update (scrobble).
   * @param userBookId  The user_book relationship ID
   * @param progressPercent  Reading progress (0.0 to 1.0)
   * @return OK on success, error code on failure
   */
  static Error updateProgress(int userBookId, float progressPercent);

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);
};
