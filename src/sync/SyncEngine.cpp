#include "SyncEngine.h"

#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include <utility>

#include "BookmarkSync.h"
#include "ReadingSessionLog.h"
#include "SyncRequest.h"
#include "clippings/ClippingSync.h"

namespace {
std::string documentIdFor(const std::string& bookPath, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(bookPath)
                                                 : KOReaderDocumentId::calculate(bookPath);
}

// Returns false only when clipping sync is enabled and failed.
bool syncExtensions(const std::string& bookPath, const std::string& documentHash) {
  // Gate before allocating buffers or contacting the server.
  const bool clippings = KOREADER_STORE.getSyncClippings();
  const bool extensions = KOREADER_STORE.getServerType() != KOReaderServerType::KOSYNC;
  if (!clippings && !extensions) return true;

  SyncRequest sync;
  if (!KOREADER_STORE.hasCredentials() || !sync.ready()) {
    LOG_ERR("SYNC", "Sync extensions skipped: no credentials or insufficient heap");
    return !clippings;
  }

  const bool clippingsSynced = !clippings || clippingSync::run(sync, bookPath, documentHash);

  // Probe first: a server that does not offer the extensions must never receive this data.
  const std::string bookmarksUrl = KOREADER_STORE.getBaseUrl() + "/api/v1/bookmarks/" + documentHash;
  const bool offered = extensions && sync.supportsDeltaSync(bookmarksUrl + "?cursor=0&limit=1");
  LOG_INF("SYNC", "Extensions: clippings=%d offered=%d", clippingsSynced, offered);
  if (offered) {
    if (!bookmarkSync::run(sync, bookPath, documentHash)) LOG_ERR("SYNC", "Bookmark sync failed");
    if (!readingSessionLog::upload(sync)) LOG_ERR("SYNC", "Reading session upload failed");
    LOG_INF("SYNC", "Bookmarks and reading sessions done");
  }
  return clippingsSynced;
}
}  // namespace

std::string syncEngine::documentId(const std::string& bookPath) {
  return documentIdFor(bookPath, KOREADER_STORE.getMatchMethod());
}

syncEngine::Pull syncEngine::pull(const std::string& bookPath, const bool smartSync) {
  Pull pull;
  const DocumentMatchMethod method = KOREADER_STORE.getMatchMethod();
  pull.documentHash = documentIdFor(bookPath, method);
  if (pull.documentHash.empty()) return pull;

  pull.extensionsSynced = syncExtensions(bookPath, pull.documentHash);
  if (!pull.extensionsSynced) return pull;

  pull.result = KOReaderSyncClient::getProgress(pull.documentHash, pull.remote);
  LOG_DBG("SYNC", "Remote progress: result=%d http=%d doc=%s", pull.result, KOReaderSyncClient::lastHttpCode,
          pull.documentHash.c_str());

  if (smartSync) {
    // The same book may have been synced under the other match method by another device.
    const std::string alternateHash =
        documentIdFor(bookPath, method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY
                                                                        : DocumentMatchMethod::FILENAME);
    KOReaderProgress alternate;
    if (!alternateHash.empty() && alternateHash != pull.documentHash &&
        KOReaderSyncClient::getProgress(alternateHash, alternate) == KOReaderSyncClient::OK) {
      if (pull.result == KOReaderSyncClient::NOT_FOUND) {
        pull.remote = std::move(alternate);
        pull.result = KOReaderSyncClient::OK;
      } else {
        pull.alternate = std::move(alternate);
      }
    }
  }
  return pull;
}

KOReaderSyncClient::Error syncEngine::push(const KOReaderProgress& progress) {
  return KOReaderSyncClient::updateProgress(progress);
}
