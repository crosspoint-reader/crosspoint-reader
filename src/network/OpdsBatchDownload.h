#pragma once
#include <OpdsClient.h>

#include <string>

#include "network/HttpDownloader.h"

/**
 * Downloads every acquisition entry of an OPDS catalog that is not already on
 * the SD card, following the feed's `next` links.
 *
 * One feed page is parsed at a time (OpdsFeedParser already caps a page at
 * OpdsLimits::MAX_ENTRIES) and only the few fields a transfer needs are kept
 * while books stream straight to file.
 */
namespace OpdsBatchDownload {

enum class Result {
  COMPLETED,      // Whole catalog walked (individual books may still have failed)
  LIMIT_REACHED,  // Stopped at MAX_ENTRIES or the page cap with books left
  CANCELLED,      // Caller asked to stop
  FAILED,         // A feed page could not be fetched or parsed
};

// Live counters, passed to the caller on every progress tick.
struct Status {
  int examined = 0;    // Acquisition entries seen so far
  int downloaded = 0;  // Books written to the SD card
  int skipped = 0;     // Books already present under their target filename
  int failed = 0;      // Books whose transfer errored (the walk continues)
  // Title of the book currently transferring; empty between books.
  const char* title = "";
};

// Caller hooks. `progress` runs before and after each book; return false to
// cancel. `transfer` downloads one book to `dest` with the catalog's auth and
// owns the per-chunk progress and cancel handling; HttpDownloader::ABORTED ends
// the walk as CANCELLED.
struct Hooks {
  void* ctx = nullptr;
  bool (*progress)(void* ctx, const Status& status) = nullptr;
  HttpDownloader::DownloadError (*transfer)(void* ctx, const std::string& url, const std::string& dest,
                                            const freeink::opds::HttpAuth& auth) = nullptr;
};

// Hard ceiling on transfers attempted in one run (skips excluded), so a large
// catalog cannot keep a battery-powered device busy forever.
constexpr int MAX_ENTRIES = 200;

// Destination path for `book` on the SD card, resolved against
// SETTINGS.opdsDownloadFolder and SETTINGS.opdsFilenameFormat (creating the
// folder, or falling back to the SD root when it cannot be created). Composed
// exactly the same way for the batch skip check and for a single download, so
// a file the browser wrote is recognised as already-present.
std::string destPath(const std::string& author, const std::string& title);

// Walks the catalog starting at `feedUrl` (absolute) through `client`, which
// carries the server's credentials and tokens, and downloads what is missing.
// Purchases and LCP-protected books are left to the per-book flow.
Result run(freeink::opds::OpdsClient& client, const std::string& feedUrl, const Hooks& hooks, Status& status);

}  // namespace OpdsBatchDownload
