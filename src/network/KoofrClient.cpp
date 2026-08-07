#include "KoofrClient.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>

#include "HalStorage.h"
#include "KoofrCredentialStore.h"

namespace {
// The TLS handshake needs working heap. These mirror the floors calibrated for
// the KOSync client (see KOReaderSyncClient.cpp): wolfSSL's largest single
// allocation is the ~17 KB record buffer, and the transient peak across the SP
// ECC + X25519 work lands around 30-40 KB. Failing the gate costs the user a
// clear "not enough memory" instead of a doomed handshake that burns 15 s.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Heap that must remain available *beyond* a file's buffer before we allocate
// it. The established TLS session keeps its record buffer alive for the whole
// upload, so a file that only just fits would starve the write path.
constexpr uint32_t UPLOAD_HEAP_MARGIN = 12000;

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOOFR", "Insufficient heap for TLS handshake: %u free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

// Percent-encode one path component. Everything outside RFC 3986's unreserved
// set is escaped: highlight file names come from book titles, so they routinely
// carry spaces, '&', '#', '+' and non-ASCII bytes that would otherwise change
// the meaning of the URL.
std::string encodeSegment(const std::string& segment) {
  // Not named HEX: Arduino's Print.h defines that as a macro for base 16.
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(segment.size() + segment.size() / 2);
  for (const char rawChar : segment) {
    const auto c = static_cast<unsigned char>(rawChar);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 0x0F];
    }
  }
  return out;
}

// Percent-encode a '/'-separated remote path, preserving the separators.
std::string encodePath(const std::string& path) {
  std::string out;
  out.reserve(path.size() + path.size() / 2);
  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == std::string::npos ? path.size() : slash;
    if (!out.empty()) out += '/';
    out += encodeSegment(path.substr(start, end - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return out;
}
}  // namespace

KoofrClient::KoofrClient() {
  http.setInsecure();
  http.setUserAgent("CrossPoint-Reader");
  // Uploading a folder of highlights is a burst of requests to one host; the
  // default keep-alive means they share a single TLS handshake.
  http.setReuse(true);
}

bool KoofrClient::beginRequest(const std::string& url) {
  if (!http.begin(url)) {
    LOG_ERR("KOOFR", "Bad URL: %s", url.c_str());
    return false;
  }
  http.setBasicAuth(KOOFR_STORE.getUsername(), KOOFR_STORE.getPassword());
  return true;
}

KoofrClient::Error KoofrClient::classify(const int httpCode) const {
  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401 || httpCode == 403) return AUTH_FAILED;
  return SERVER_ERROR;
}

KoofrClient::Error KoofrClient::ensureRemoteDir() {
  lastHttpCode = 0;
  if (!KOOFR_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string baseUrl = KOOFR_STORE.getBaseUrl();
  const std::string remoteDir = KOOFR_STORE.getEffectiveRemoteDir();

  // MKCOL only creates one level, and fails with 409 when the parent is
  // missing, so walk the path creating each ancestor in turn.
  std::string built;
  size_t start = 0;
  while (start <= remoteDir.size()) {
    const size_t slash = remoteDir.find('/', start);
    const size_t end = slash == std::string::npos ? remoteDir.size() : slash;
    const std::string segment = remoteDir.substr(start, end - start);

    if (!segment.empty()) {
      if (!built.empty()) built += '/';
      built += segment;

      const std::string url = baseUrl + "/" + encodePath(built) + "/";
      if (!beginRequest(url)) return NETWORK_ERROR;
      const int httpCode = http.sendRequest("MKCOL", nullptr, 0);
      lastHttpCode = httpCode;
      LOG_DBG("KOOFR", "MKCOL %s -> %d", built.c_str(), httpCode);

      // 405 Method Not Allowed is WebDAV's "collection already exists", which
      // is the common case on every sync after the first.
      if (httpCode != 405) {
        const Error error = classify(httpCode);
        if (error != OK) {
          LOG_ERR("KOOFR", "MKCOL %s failed: %d", built.c_str(), httpCode);
          return error;
        }
      }
    }

    if (slash == std::string::npos) break;
    start = slash + 1;
  }

  return OK;
}

KoofrClient::Error KoofrClient::uploadFile(const char* localPath, const std::string& remoteName) {
  lastHttpCode = 0;
  if (!KOOFR_STORE.hasCredentials()) return NO_CREDENTIALS;

  size_t size = 0;
  std::unique_ptr<uint8_t[]> body;

  // Scoped so the HalFile destructor releases the SD handle before the upload:
  // the PUT can take seconds, and nothing below needs the card.
  {
    HalFile file;
    if (!Storage.openFileForRead("KOOFR", localPath, file)) {
      return FILE_ERROR;
    }

    size = file.fileSize();
    if (size > MAX_UPLOAD_BYTES) {
      LOG_ERR("KOOFR", "%s is %u bytes, over the %u byte limit", localPath, (unsigned)size, (unsigned)MAX_UPLOAD_BYTES);
      return FILE_TOO_LARGE;
    }

    // SecureHttpClient writes a request body from one contiguous buffer (it has
    // no streaming upload), so the whole file has to be resident. Check the heap
    // can spare it *and* keep the live TLS session working before allocating.
    if (ESP.getMaxAllocHeap() < size + UPLOAD_HEAP_MARGIN) {
      LOG_ERR("KOOFR", "Not enough heap for %u byte body (%u max alloc)", (unsigned)size,
              (unsigned)ESP.getMaxAllocHeap());
      return LOW_MEMORY;
    }

    body = makeUniqueNoThrow<uint8_t[]>(size);
    if (!body) {
      LOG_ERR("KOOFR", "OOM: %u bytes for %s", (unsigned)size, localPath);
      return LOW_MEMORY;
    }

    // A short read means the file changed or the card faulted; uploading a
    // truncated body would silently replace good remote content with bad.
    if (size > 0 && file.read(body.get(), size) != static_cast<int>(size)) {
      LOG_ERR("KOOFR", "Short read: %s", localPath);
      return FILE_ERROR;
    }
  }

  const std::string url = KOOFR_STORE.getBaseUrl() + "/" + encodePath(KOOFR_STORE.getEffectiveRemoteDir()) + "/" +
                          encodeSegment(remoteName);
  LOG_DBG("KOOFR", "PUT %s (%u bytes, heap: %u)", remoteName.c_str(), (unsigned)size, (unsigned)ESP.getFreeHeap());
  if (!beginRequest(url)) return NETWORK_ERROR;

  http.addHeader("Content-Type", "text/markdown; charset=utf-8");
  // SecureHttpClient only emits Content-Length for a non-empty payload, and a
  // keep-alive PUT with neither length nor body leaves the server waiting.
  if (size == 0) http.addHeader("Content-Length", "0");
  const int httpCode = http.sendRequest("PUT", body.get(), size);
  lastHttpCode = httpCode;
  LOG_DBG("KOOFR", "PUT %s -> %d", remoteName.c_str(), httpCode);

  return classify(httpCode);
}

const char* KoofrClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return tr(STR_KOOFR_UPLOAD_COMPLETE);
    case NO_CREDENTIALS:
      return tr(STR_NO_CREDENTIALS_MSG);
    case NETWORK_ERROR:
      return tr(STR_NETWORK_ERROR);
    case AUTH_FAILED:
      return tr(STR_AUTH_FAILED);
    case SERVER_ERROR:
      return tr(STR_SERVER_ERROR);
    case LOW_MEMORY:
      return tr(STR_LOW_MEMORY_RETRY);
    case FILE_ERROR:
      return tr(STR_KOOFR_FILE_READ_FAILED);
    case FILE_TOO_LARGE:
      return tr(STR_KOOFR_FILE_TOO_LARGE);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
