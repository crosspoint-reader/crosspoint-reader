#include "OpdsBatchDownload.h"

#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsFeedParser.h>
#include <ResumableFetch.h>

#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace {
// Bounds the walk, skipped books included, and stops a `next` link cycle.
constexpr int MAX_PAGES = 8;

// The fields a transfer needs, copied out of the page so the parsed feed can
// be freed before TLS claims its buffers.
struct PendingBook {
  std::string title;
  std::string author;
  std::string href;
  bool indirect = false;
};

// A purchase link (or any misbehaving endpoint) can answer 200 with an HTML
// page; a real EPUB is a ZIP container.
bool looksLikeEpub(const std::string& path) {
  uint8_t magic[4] = {0};
  HalFile check;
  if (Storage.openFileForRead("OPDS", path.c_str(), check)) {
    check.read(magic, sizeof(magic));
    check.close();
  }
  return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
}
}  // namespace

namespace OpdsBatchDownload {

std::string destPath(const std::string& author, const std::string& title) {
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }
  // Titles are unbounded (a fixed char[] would truncate). Cold path (a
  // multi-second download follows), so one reserve'd owning string is right.
  std::string path;
  path.reserve(96);
  if (haveFolder) path += folder;
  path += '/';
  path += opdsBookFilename(author, title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

bool authRetryAllowed(const HttpDownloader::DownloadError result, const std::string& url,
                      const std::string& serverUrl) {
  return result == HttpDownloader::UNAUTHORIZED && HttpDownloader::lastStatus() == 401 &&
         !HttpDownloader::lastAnswerRedirected() && freeink::fetchOrigin(url) == freeink::fetchOrigin(serverUrl);
}

Result run(freeink::opds::OpdsClient& client, const std::string& serverUrl, const std::string& feedUrl,
           const Hooks& hooks, Status& status) {
  if (feedUrl.empty() || !hooks.transfer) return Result::FAILED;
  const auto notify = [&hooks, &status] { return hooks.progress ? hooks.progress(hooks.ctx, status) : true; };

  std::string pageUrl = feedUrl;
  for (int page = 0; page < MAX_PAGES; page++) {
    LOG_DBG("OPDS", "Batch page %d: %s", page, pageUrl.c_str());

    std::string nextHref;
    std::vector<PendingBook> books;
    {
      OpdsFeedParser parser;
      if (client.fetchFeed(pageUrl, parser) != freeink::opds::OpdsClient::FetchStatus::Ok) return Result::FAILED;
      nextHref = parser.getNextPageUrl();
      for (auto& entry : parser.takeEntries()) {
        // Buying is never implied by "download new", and LCP books need the
        // per-book passphrase flow.
        if (entry.type != OpdsEntryType::BOOK || entry.purchase || entry.lcpLicense) continue;
        books.push_back({std::move(entry.title), std::move(entry.author), std::move(entry.href), entry.indirect});
      }
    }

    for (const auto& book : books) {
      if (status.downloaded + status.failed >= MAX_ENTRIES) return Result::LIMIT_REACHED;
      status.examined++;

      const std::string filename = destPath(book.author, book.title);
      if (Storage.exists(filename.c_str())) {
        status.skipped++;
        if (!notify()) return Result::CANCELLED;
        continue;
      }

      status.title = book.title.c_str();  // Borrowed for this iteration only
      if (!notify()) {
        status.title = "";
        return Result::CANCELLED;
      }

      // Resolve against the page it came from, like the browser does.
      std::string downloadUrl = UrlUtils::buildUrl(pageUrl, book.href);
      bool resolved = true;
      if (book.indirect) {
        std::string direct;
        bool isEpub = false;
        resolved = client.resolveIndirect(downloadUrl, direct, isEpub);
        if (resolved) downloadUrl = std::move(direct);
      }

      auto result = resolved ? hooks.transfer(hooks.ctx, downloadUrl, filename, client.downloadAuth())
                             : HttpDownloader::HTTP_ERROR;
      if (authRetryAllowed(result, downloadUrl, serverUrl)) {
        if (!notify()) return Result::CANCELLED;
        if (client.authenticateDownload(downloadUrl)) {
          result = hooks.transfer(hooks.ctx, downloadUrl, filename, client.downloadAuth());
        }
      }
      status.title = "";
      if (result == HttpDownloader::OK && looksLikeEpub(filename)) {
        status.downloaded++;
        clearBookCache(filename);
        library::markLibraryIndexDirty();
      } else if (result == HttpDownloader::ABORTED) {
        return Result::CANCELLED;
      } else {
        // One bad book must not end the sync; the rest of the catalog is still
        // worth fetching.
        LOG_ERR("OPDS", "Batch download failed (%d): %s", static_cast<int>(result), filename.c_str());
        if (result == HttpDownloader::OK) Storage.remove(filename.c_str());
        status.failed++;
      }
      if (!notify()) return Result::CANCELLED;
    }

    if (nextHref.empty()) return Result::COMPLETED;
    pageUrl = UrlUtils::buildUrl(pageUrl, nextHref);
  }

  return Result::LIMIT_REACHED;
}

}  // namespace OpdsBatchDownload
