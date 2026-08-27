#include "OpdsBatchDownload.h"

#include <HalStorage.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <OpdsStream.h>

#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace {
// A page carries at most 62 entries (OpdsParser's storage cap), so this bounds
// the walk well above MAX_ENTRIES while still stopping a `next` link cycle.
constexpr int MAX_PAGES = 8;
}  // namespace

namespace OpdsBatchDownload {

std::string destPath(const std::string& author, const std::string& title, const bool haveFolder) {
  // Mirrors OpdsBookBrowserActivity::downloadBook(): folder prefix, '/', then
  // opdsBookFilename() under the configured format.
  std::string path;
  path.reserve(96);
  if (haveFolder) path += SETTINGS.opdsDownloadFolder;
  path += '/';
  path += opdsBookFilename(author, title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

bool ensureDownloadFolder() {
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  if (folder[0] == '\0') return false;
  // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
  // existing caller checks exists() before mkdir. On real failure, fall back to
  // SD root so the download is never lost.
  if (Storage.exists(folder) || Storage.mkdir(folder)) return true;
  LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
  return false;
}

Result run(const OpdsServer& server, const std::string& startPath, const Observer& observer, Status& status) {
  if (server.url.empty()) return Result::FAILED;

  const bool haveFolder = ensureDownloadFolder();
  const auto notify = [&observer, &status] { return observer.fn ? observer.fn(observer.ctx, status) : true; };

  std::string path = startPath;
  for (int page = 0; page < MAX_PAGES && status.examined < MAX_ENTRIES; page++) {
    const std::string feedUrl = UrlUtils::buildUrl(server.url, path);
    LOG_DBG("OPDS", "Batch page %d: %s", page, feedUrl.c_str());

    std::string nextHref;
    std::vector<OpdsEntry> entries;
    {
      OpdsParser parser;
      {
        OpdsParserStream stream{parser};
        if (!HttpDownloader::fetchUrl(feedUrl, stream, server.username, server.password)) return Result::FAILED;
      }
      if (!parser) return Result::FAILED;
      nextHref = parser.getNextPageUrl();
      entries = std::move(parser).getEntries();
    }

    for (const auto& entry : entries) {
      if (entry.type != OpdsEntryType::BOOK) continue;
      if (status.examined >= MAX_ENTRIES) break;
      status.examined++;

      const std::string filename = destPath(entry.author, entry.title, haveFolder);
      if (Storage.exists(filename.c_str())) {
        status.skipped++;
        if (!notify()) return Result::CANCELLED;
        continue;
      }

      status.title = entry.title.c_str();  // Borrowed for this iteration only
      status.bytes = status.bytesTotal = 0;
      if (!notify()) {
        status.title = "";
        return Result::CANCELLED;
      }

      bool cancelled = false;
      const std::string downloadUrl = UrlUtils::buildUrl(feedUrl, entry.href);
      const auto result = HttpDownloader::downloadToFile(
          downloadUrl, filename,
          [&status, &notify, &cancelled](const size_t downloaded, const size_t total) {
            status.bytes = downloaded;
            status.bytesTotal = total;
            if (!notify()) cancelled = true;
          },
          &cancelled, server.username, server.password);

      status.title = "";
      status.bytes = status.bytesTotal = 0;
      if (result == HttpDownloader::OK) {
        status.downloaded++;
        clearBookCache(filename);
      } else if (result == HttpDownloader::ABORTED) {
        return Result::CANCELLED;
      } else {
        // One bad book must not end the sync; the rest of the catalog is still
        // worth fetching.
        LOG_ERR("OPDS", "Batch download failed (%d): %s", static_cast<int>(result), filename.c_str());
        status.failed++;
      }
      if (!notify()) return Result::CANCELLED;
    }

    if (nextHref.empty()) break;
    // Resolve the next page against the page it came from, like navigateToEntry().
    path = UrlUtils::buildUrl(feedUrl, nextHref);
  }

  return Result::COMPLETED;
}

}  // namespace OpdsBatchDownload
