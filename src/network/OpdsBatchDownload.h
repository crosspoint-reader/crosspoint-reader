#pragma once
#include <cstddef>
#include <string>

#include "OpdsServerStore.h"

/**
 * Downloads every acquisition entry of an OPDS catalog that is not already on
 * the SD card, following the feed's `next` links.
 *
 * Nothing is buffered: one feed page is parsed at a time (OpdsParser already
 * caps a page at 62 entries) and each book streams straight to file through
 * HttpDownloader::downloadToFile.
 */
namespace OpdsBatchDownload {

enum class Result {
  COMPLETED,  // Whole catalog walked (individual books may still have failed)
  CANCELLED,  // Observer asked to stop
  FAILED,     // A feed page could not be fetched or parsed
};

// Live counters, passed to the observer on every progress tick.
struct Status {
  int examined = 0;    // Acquisition entries seen so far
  int downloaded = 0;  // Books written to the SD card
  int skipped = 0;     // Books already present under their target filename
  int failed = 0;      // Books whose transfer errored (the walk continues)
  // Title of the book currently transferring; empty between books.
  const char* title = "";
  size_t bytes = 0;       // Bytes of the current transfer
  size_t bytesTotal = 0;  // Content-Length of the current transfer, 0 if unknown
};

// Called between chunks and between books. Return false to cancel; the partial
// file of an in-flight transfer is removed by HttpDownloader.
struct Observer {
  void* ctx = nullptr;
  bool (*fn)(void* ctx, const Status& status) = nullptr;
};

// Hard ceiling on acquisition entries examined in one run, so a large or
// self-referential catalog cannot walk forever on a battery-powered device.
constexpr int MAX_ENTRIES = 200;

// Destination path for `book` on the SD card, resolved against
// SETTINGS.opdsDownloadFolder and SETTINGS.opdsFilenameFormat. `haveFolder`
// false forces the SD root (the caller's mkdir fallback). Composed exactly the
// same way for the batch skip check and for a single download, so a file the
// browser wrote is recognised as already-present.
std::string destPath(const std::string& author, const std::string& title, bool haveFolder);

// Ensures SETTINGS.opdsDownloadFolder exists; returns false when it is unset or
// could not be created, meaning downloads fall back to the SD root.
bool ensureDownloadFolder();

// Walks the catalog starting at `startPath` (relative to server.url, empty for
// the catalog root) and downloads what is missing.
Result run(const OpdsServer& server, const std::string& startPath, const Observer& observer, Status& status);

}  // namespace OpdsBatchDownload
