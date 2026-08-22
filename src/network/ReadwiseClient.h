#pragma once

#include <cstddef>
#include <vector>

class EpubComposer;

/**
 * Readwise Reader API client (https://readwise.io/reader_api).
 * Streams responses through chunked JSON parsing so articles of any size pass
 * through without buffering. Listing is exposed per page so the caller can
 * download documents between pages instead of stacking two HTTP connections.
 */
class ReadwiseClient {
 public:
  static constexpr char FOLDER[] = "/Readwise";

  enum Result {
    OK = 0,
    AUTH_FAILED,
    HTTP_ERROR,
    CANCELLED,
  };

  struct DocMeta {
    char id[40];
    char title[128];
  };

  // Metadata captured while streaming a single document's content.
  struct DocumentContent {
    char title[128];
    char author[64];
  };

  static const char* errorString(Result result);

  /**
   * Fetches one page (up to 100 docs) of documents carrying `tag`.
   * Pass empty `cursor` for the first page; on success `nextCursorOut` holds
   * the continuation cursor (empty string when done).
   */
  static Result listPage(const char* apiKey, const char* tag, const char* cursor, std::vector<DocMeta>& outItems,
                         char* nextCursorOut, size_t nextCursorSize, const bool* cancelFlag);

  /**
   * Fetches one document with html_content, streams the HTML through
   * `composer` (begin()/beginContent() must already have run), and captures
   * title/author into `outMeta` (they may arrive before or after the bytes).
   */
  static Result downloadDocument(const char* apiKey, const char* docId, EpubComposer& composer,
                                 DocumentContent& outMeta, const bool* cancelFlag);

  /**
   * Builds "<FOLDER>/<sanitized title>_<first 8 id chars>.epub". The id suffix
   * makes existence checks an exact dedupe signal even across title clashes.
   */
  static void buildEpubPath(const char* title, const char* docId, char* out, size_t outSize);
};
