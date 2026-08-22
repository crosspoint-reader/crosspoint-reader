#include "ReadwiseClient.h"

#include <HalStorage.h>
#include "HttpDownloader.h"
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "EpubComposer.h"
#include "ReadwiseJsonParser.h"

namespace {
constexpr char API_BASE[] = "https://readwise.io/api/v3/list/?";
constexpr size_t URL_BUF_SIZE = 512;

// Percent-encodes everything outside the RFC 3986 unreserved set.
void urlEncode(const char* src, char* dst, size_t dstSize) {
  // Named URL_HEX because Arduino's Print.h #defines HEX.
  constexpr char URL_HEX[] = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0' && o + 4 < dstSize; i++) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      dst[o++] = static_cast<char>(c);
    } else {
      dst[o++] = '%';
      dst[o++] = URL_HEX[c >> 4];
      dst[o++] = URL_HEX[c & 0xF];
    }
  }
  dst[o] = '\0';
}

// Appends a non-null-terminated chunk to a fixed buffer, truncating safely.
void appendTo(char* dst, const size_t dstSize, const char* chunk, const size_t len) {
  const size_t used = strlen(dst);
  if (used + 1 >= dstSize || len == 0) return;
  const size_t space = dstSize - used - 1;
  const size_t n = len < space ? len : space;
  memcpy(dst + used, chunk, n);
  dst[used + n] = '\0';
}

struct RequestState {
  std::vector<ReadwiseClient::DocMeta>* items = nullptr;
  ReadwiseClient::DocMeta current{};
  char nextCursor[192] = {0};
  EpubComposer* composer = nullptr;   // download mode when non-null
  ReadwiseClient::DocumentContent* meta = nullptr;
  bool aborted = false;
};

int onRequestField(void* ctx, const char* key, const char* chunk, const size_t len, const bool finalChunk) {
  auto* st = static_cast<RequestState*>(ctx);
  (void)finalChunk;

  if (st->composer != nullptr) {
    if (strcmp(key, "html_content") == 0) {
      if (len > 0 && !st->composer->contentChunk(chunk, len)) {
        st->aborted = true;
        return 1;  // abort the transfer
      }
    } else if (strcmp(key, "title") == 0) {
      appendTo(st->meta->title, sizeof(st->meta->title), chunk, len);
    } else if (strcmp(key, "author") == 0) {
      appendTo(st->meta->author, sizeof(st->meta->author), chunk, len);
    }
    return 0;
  }

  if (strcmp(key, "nextPageCursor") == 0) {
    appendTo(st->nextCursor, sizeof(st->nextCursor), chunk, len);
  } else if (strcmp(key, "id") == 0) {
    appendTo(st->current.id, sizeof(st->current.id), chunk, len);
  } else if (strcmp(key, "title") == 0) {
    appendTo(st->current.title, sizeof(st->current.title), chunk, len);
  }
  return 0;
}

void onRequestDocumentStart(void* ctx) {
  auto* st = static_cast<RequestState*>(ctx);
  if (st->composer != nullptr) return;
  st->current.id[0] = '\0';
  st->current.title[0] = '\0';
}

void onRequestDocumentEnd(void* ctx) {
  auto* st = static_cast<RequestState*>(ctx);
  if (st->composer != nullptr || st->items == nullptr) return;
  if (st->current.id[0] == '\0') return;
  st->items->push_back(st->current);
}
}  // namespace

const char* ReadwiseClient::errorString(const Result result) {
  switch (result) {
    case OK:
      return "ok";
    case AUTH_FAILED:
      return "auth failed";
    case CANCELLED:
      return "cancelled";
    default:
      return "network error";
  }
}

ReadwiseClient::Result ReadwiseClient::listPage(const char* apiKey, const char* tag, const char* cursor,
                                                std::vector<DocMeta>& outItems, char* const nextCursorOut,
                                                const size_t nextCursorSize, const bool* cancelFlag) {
  if (cancelFlag != nullptr && *cancelFlag) return CANCELLED;

  char tagEnc[128];
  urlEncode(tag, tagEnc, sizeof(tagEnc));
  char cursorEnc[256];
  cursorEnc[0] = '\0';
  if (cursor[0] != '\0') {
    urlEncode(cursor, cursorEnc, sizeof(cursorEnc));
  }

  char url[URL_BUF_SIZE];
  if (cursorEnc[0] != '\0') {
    snprintf(url, sizeof(url), "%stag=%s&limit=100&pageCursor=%s", API_BASE, tagEnc, cursorEnc);
  } else {
    snprintf(url, sizeof(url), "%stag=%s&limit=100", API_BASE, tagEnc);
  }

  RequestState state;
  state.items = &outItems;
  outItems.reserve(100);

  ReadwiseJsonParser::Callbacks callbacks = {};
  callbacks.ctx = &state;
  callbacks.onField = onRequestField;
  callbacks.onDocumentStart = onRequestDocumentStart;
  callbacks.onDocumentEnd = onRequestDocumentEnd;
  ReadwiseJsonParser parser(callbacks);

  HttpDownloader::HeaderList headers;
  headers.emplace_back("Authorization", std::string("Token ") + apiKey);

  LOG_INF("RWISE", "Listing: %s", url);
  const bool fetched =
      HttpDownloader::fetchUrl(url, [&parser](const uint8_t* data, size_t len) {
        parser.feed(reinterpret_cast<const char*>(data), len);
        return !parser.hasError();
      }, headers);
  if (!fetched || parser.hasError()) {
    LOG_ERR("RWISE", "List request failed (http ok: %d, json error: %d)", fetched, parser.hasError());
    return HTTP_ERROR;
  }

  nextCursorOut[0] = '\0';
  strncpy(nextCursorOut, state.nextCursor, nextCursorSize - 1);
  nextCursorOut[nextCursorSize - 1] = '\0';
  return OK;
}

ReadwiseClient::Result ReadwiseClient::downloadDocument(const char* apiKey, const char* docId,
                                                        EpubComposer& composer, DocumentContent& outMeta,
                                                        const bool* cancelFlag) {
  if (cancelFlag != nullptr && *cancelFlag) return CANCELLED;

  char url[URL_BUF_SIZE];
  snprintf(url, sizeof(url), "%sid=%s&withHtmlContent=true", API_BASE, docId);

  RequestState state;
  state.composer = &composer;
  state.meta = &outMeta;

  ReadwiseJsonParser::Callbacks callbacks = {};
  callbacks.ctx = &state;
  callbacks.onField = onRequestField;
  callbacks.onDocumentStart = onRequestDocumentStart;
  callbacks.onDocumentEnd = onRequestDocumentEnd;
  ReadwiseJsonParser parser(callbacks);

  HttpDownloader::HeaderList headers;
  headers.emplace_back("Authorization", std::string("Token ") + apiKey);

  LOG_INF("RWISE", "Fetching content: %s", url);
  const bool fetched =
      HttpDownloader::fetchUrl(url, [&parser, cancelFlag](const uint8_t* data, size_t len) {
        if (cancelFlag != nullptr && *cancelFlag) return false;
        parser.feed(reinterpret_cast<const char*>(data), len);
        return !parser.hasError();
      }, headers);
  if (!fetched || parser.hasError() || state.aborted) {
    LOG_ERR("RWISE", "Download failed (http ok: %d, json error: %d)", fetched, parser.hasError());
    return (cancelFlag != nullptr && *cancelFlag) ? CANCELLED : HTTP_ERROR;
  }
  return OK;
}

void ReadwiseClient::buildEpubPath(const char* title, const char* docId, char* out, const size_t outSize) {
  // Keep letters/digits/space/-/_ and whole UTF-8 sequences; everything else
  // collapses to '_'. Truncated to leave room for "_<id8>.epub".
  constexpr size_t MAX_BASE_BYTES = 48;
  char base[MAX_BASE_BYTES + 1];
  size_t o = 0;
  for (size_t i = 0; title[i] != '\0' && o < MAX_BASE_BYTES;) {
    const unsigned char c = static_cast<unsigned char>(title[i]);
    const bool safeChar = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == ' ';
    if (safeChar) {
      base[o++] = static_cast<char>(c);
      i++;
    } else if (c < 0x80) {
      base[o++] = '_';
      i++;
    } else {
      // Copy one whole UTF-8 sequence.
      const size_t len = strlen(title);
      size_t seq = 1;
      while (i + seq < len && seq < 4 && (static_cast<unsigned char>(title[i + seq]) & 0xC0) == 0x80) seq++;
      if (o + seq > MAX_BASE_BYTES) break;
      memcpy(base + o, title + i, seq);
      o += seq;
      i += seq;
    }
  }
  while (o > 0 && base[o - 1] == '_') o--;  // trim trailing separators
  base[o] = '\0';

  snprintf(out, outSize, "%s/%s_%.8s.epub", FOLDER, base, docId);
}
