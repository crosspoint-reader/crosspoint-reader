#include "RangeDownload.h"

#include <Arduino.h>  // millis()
#include <Logging.h>

#include <algorithm>

namespace crosspoint {

namespace {

namespace hr = http_range;
using C = SecureHttpClient;

// The response as one streamed body: every byte the server sends goes to the sink.
int streamGet(SecureHttpClient& http, const std::string& url, DownloadSink& sink) {
  auto body = [&](const uint8_t* data, size_t len) -> bool {
    if (!sink.write(data, len)) return false;  // abort
    sink.downloaded += len;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) return false;
    }
    return true;
  };
  auto progress = [&](size_t /*downloaded*/, size_t total) -> bool {
    sink.total = total;
    return true;
  };
  return http.get(url, body, progress);
}

bool isTransportFailure(int rc) {
  return rc == C::ERR_CONNECT || rc == C::ERR_SEND || rc == C::ERR_TIMEOUT || rc == C::ERR_TRUNCATED;
}

bool isHttps(const std::string& url) {
  static constexpr char SCHEME[] = "https://";
  if (url.size() < sizeof(SCHEME) - 1) return false;
  for (size_t i = 0; i + 1 < sizeof(SCHEME); ++i) {
    char c = url[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != SCHEME[i]) return false;
  }
  return true;
}

// Fetches url in Range requests, so that no response, and so no TLS record, is larger than a chunk
// plus its headers (HttpRange.h has why and the sizing). Each request is sized from the largest free
// block just before it, starting from firstLargest, which the caller measured, and never above the
// session's ceiling. An out-of-memory read lowers that ceiling for good. Returns 200 once the whole
// file has been written, else the failing HTTP status or SecureHttpError.
//
// The handling of a server that ignores Range (200 instead of 206: rewind the sink, take the whole
// body) and the resume from the bytes already received are adapted from Free-Ink/freeink-sdk f80a99c
// (ResumableFetch.h, Justin Mitchell). Different here: every request is a bounded range on the same
// kept-alive connection, the total comes from Content-Range, and chunks after the first go straight
// to the URL the first one was redirected to (and back through url when that target starts refusing).
int chunkedGet(SecureHttpClient& http, const std::string& url, DownloadSink& sink, size_t firstLargest,
               ChunkSession& session, LargestFreeBlockFn largestFreeBlock) {
  struct RequestModeGuard {
    C& client;
    ~RequestModeGuard() {
      client.clearRange();
      client.setQuietRequests(false);
      client.clearCredentialScope();
    }
  } guard{http};
  http.setQuietRequests(true);
  // The chunks sent straight to a redirect target must not carry url's credentials there: they
  // stay url's, as they were on the redirect itself.
  http.setCredentialScope(url);

  const unsigned long startMs = millis();
  std::string target = url;
  bool targetRefreshed = false;  // target went back to url since the last chunk that arrived
  size_t offset = 0;
  bool totalKnown = false;
  size_t total = 0;
  unsigned requests = 0;
  bool wholeBody = false;
  hr::ChunkSizer sizer(session.ceiling);
  hr::RetryBudget retries;
  size_t chunkSize = sizer.next(firstLargest);
  size_t smallest = chunkSize;
  size_t largestUsed = chunkSize;
  bool outOfMemory = false;

  hr::Chunk chunk;
  while (true) {
    if (requests > 0) {
      // The heap fragments as the download runs: size this request from the block there is now.
      const size_t largest = largestFreeBlock();
      const size_t previous = chunkSize;
      chunkSize = sizer.next(largest);
      if (chunkSize != previous) {
        LOG_DBG("HTTP", "chunk size %u -> %u B at %u B (largest free block %u B%s)", static_cast<unsigned>(previous),
                static_cast<unsigned>(chunkSize), static_cast<unsigned>(offset), static_cast<unsigned>(largest),
                outOfMemory ? ", after an out-of-memory read" : "");
      }
      smallest = std::min(smallest, chunkSize);
      largestUsed = std::max(largestUsed, chunkSize);
      outOfMemory = false;
    }
    if (!hr::nextChunk(offset, chunkSize, totalKnown, total, chunk)) break;
    http.setRange(chunk.first, chunk.last);
    hr::RangeReply reply = hr::RangeReply::Error;
    bool classified = false;
    bool rewindFailed = false;
    bool totalChanged = false;
    size_t received = 0;
    // The server ignored Range: what it sends is the whole file from byte 0, so drop what is there.
    // From now on the session streams (one attempt per file, as before chunking existed).
    auto startWholeBody = [&]() -> bool {
      session.rangeUnsupported = true;
      if (offset > 0 && (!sink.rewind || !sink.rewind())) {
        rewindFailed = true;
        return false;
      }
      offset = 0;
      sink.downloaded = 0;
      totalKnown = http.lastContentLength() >= 0;
      total = totalKnown ? static_cast<size_t>(http.lastContentLength()) : 0;
      return true;
    };
    auto body = [&](const uint8_t* data, size_t len) -> bool {
      if (!classified) {
        classified = true;
        reply = hr::classifyReply(http.lastStatus(), http.lastHasContentRange(), http.lastContentRange(), chunk.first);
        if (reply == hr::RangeReply::WholeBody) {
          if (!startWholeBody()) return false;
        } else if (reply == hr::RangeReply::Partial && http.lastContentRange().totalKnown) {
          // A different size is a different version of the file: never splice it onto this one.
          if (totalKnown && http.lastContentRange().total != total) {
            totalChanged = true;
            return false;
          }
          totalKnown = true;
          total = http.lastContentRange().total;
        }
      }
      // An error page (or a range we did not ask for) is drained, never written into the file.
      if (reply != hr::RangeReply::Partial && reply != hr::RangeReply::WholeBody) return true;
      if (!sink.write(data, len)) return false;
      offset += len;
      received += len;
      sink.downloaded = offset;
      sink.total = totalKnown ? total : 0;
      if (sink.progress && sink.total > 0 && !sink.progress(sink.downloaded, sink.total)) return false;
      return true;
    };

    const int rc = http.get(target, body, nullptr);
    ++requests;
    if (rewindFailed) {
      LOG_ERR("HTTP", "chunked download: could not restart the file for a server that ignores Range");
      return ERR_REWIND;
    }
    if (totalChanged) {
      LOG_ERR("HTTP", "chunked download: the file changed on the server at %u B (size %u -> %u B)",
              static_cast<unsigned>(offset), static_cast<unsigned>(total),
              static_cast<unsigned>(http.lastContentRange().total));
      return ERR_RESOURCE_CHANGED;
    }
    if (rc == C::ERR_ABORTED) return rc;
    if (rc >= 0 && !classified) {  // a response without a body: a 416, or an empty 206 or 200
      reply = hr::classifyReply(rc, http.lastHasContentRange(), http.lastContentRange(), chunk.first);
      if (reply == hr::RangeReply::WholeBody && !startWholeBody()) return ERR_REWIND;
    }

    // A 200 is the whole file in one response. It gets the one attempt streaming always had: a
    // reconnect would ask for a range again, get the whole file again, and start over from byte 0.
    if (reply == hr::RangeReply::WholeBody) {
      if (rc < 0) {
        LOG_ERR("HTTP", "server ignored Range; its whole-file reply failed at %u B: rc=%d",
                static_cast<unsigned>(offset), rc);
        return rc;
      }
      wholeBody = true;
      break;
    }

    // A failed request: the connection broke, or a 206 brought no bytes. Either way nothing new may
    // count as progress; what was written stays, and the next request starts after it.
    if (rc < 0 || (reply == hr::RangeReply::Partial && received == 0)) {
      outOfMemory = http.lastReadOutOfMemory();
      if (outOfMemory) {
        sizer.onOutOfMemory();
        if (sizer.ceiling() != session.ceiling) {
          LOG_DBG("HTTP", "chunk ceiling %u -> %u B for the rest of the session (out-of-memory read at %u B)",
                  static_cast<unsigned>(session.ceiling), static_cast<unsigned>(sizer.ceiling()),
                  static_cast<unsigned>(offset));
          session.ceiling = sizer.ceiling();
        }
      }
      const size_t fileBytes = totalKnown ? total : offset;
      const bool retryable = rc >= 0 || isTransportFailure(rc);
      if (!retryable || !retries.reconnectAfterFailure(received > 0, fileBytes)) {
        LOG_ERR("HTTP", "chunked download stopped at %u B after %u requests, %u reconnect(s): rc=%d%s",
                static_cast<unsigned>(offset), requests, retries.reconnects(), rc,
                outOfMemory ? " (out of memory)" : (rc >= 0 ? " (empty reply)" : ""));
        return rc < 0 ? rc : C::ERR_TRUNCATED;
      }
      // Reconnect (a full TLS handshake) and measure the heap with the new connection in
      // place, at the top of the loop. A connect that fails counts as another stall.
      http.close();
      while (!http.open(target)) {
        if (!retries.reconnectAfterFailure(false, fileBytes)) {
          LOG_ERR("HTTP", "chunked download stopped at %u B: reconnect failed", static_cast<unsigned>(offset));
          return C::ERR_CONNECT;
        }
      }
      continue;
    }

    if (reply == hr::RangeReply::Unsatisfiable) {
      // The byte after the last one written is past the end: a file of unknown size is complete,
      // provided the 416 reports the size that was written.
      const hr::ContentRange& range = http.lastContentRange();
      const bool sizeAgrees = !http.lastHasContentRange() || !range.totalKnown || range.total == offset;
      if (offset > 0 && !totalKnown && sizeAgrees) break;
    }
    // A redirect target that starts refusing is most likely a presigned CDN URL past its expiry
    // (GitHub's release assets then answer 403). Ask through the original URL, which redirects
    // afresh: once per stretch of progress, so a target that keeps refusing still fails.
    if (reply != hr::RangeReply::Partial && rc >= 400 && rc < 500 && rc != 416 && target != url && !targetRefreshed) {
      LOG_DBG("HTTP", "chunked download: redirect target answered %d at %u B; asking %s again", rc,
              static_cast<unsigned>(offset), url.c_str());
      target = url;
      targetRefreshed = true;
      continue;
    }
    if (reply != hr::RangeReply::Partial) {
      LOG_ERR("HTTP", "chunked download: unexpected reply %d to bytes %u-%u", rc, static_cast<unsigned>(chunk.first),
              static_cast<unsigned>(chunk.last));
      return rc;
    }
    target = http.lastUrl();  // follow a redirect once, not once per chunk
    targetRefreshed = false;
    retries.onProgress();
    if (hr::transferComplete(offset, totalKnown, total)) break;
  }

  LOG_DBG("HTTP", "chunked download done: %u B in %u requests, %lu ms, %u reconnect(s), chunks %u-%u B%s",
          static_cast<unsigned>(offset), requests, millis() - startMs, retries.reconnects(),
          static_cast<unsigned>(smallest), static_cast<unsigned>(largestUsed),
          wholeBody ? " (server ignored Range: whole file streamed)" : "");
  return 200;
}

}  // namespace

int downloadToSink(SecureHttpClient& http, const std::string& url, DownloadSink& sink, ChunkSession& session,
                   LargestFreeBlockFn largestFreeBlock) {
  if (!sink.rewind) return streamGet(http, url, sink);
  if (!isHttps(url)) {
    LOG_DBG("HTTP", "download mode: streamed (plain http)");
    return streamGet(http, url, sink);
  }
  if (session.rangeUnsupported) {
    LOG_DBG("HTTP", "download mode: streamed (this server ignores Range)");
    return streamGet(http, url, sink);
  }
  if (!http.open(url)) return C::ERR_CONNECT;
  const size_t largest = largestFreeBlock();
  if (!hr::shouldChunk(largest)) {
    LOG_DBG("HTTP", "download mode: streamed (largest free block %u B)", static_cast<unsigned>(largest));
    return streamGet(http, url, sink);
  }
  LOG_DBG("HTTP", "download mode: chunked %u B (largest free block %u B, ceiling %u B)",
          static_cast<unsigned>(hr::ChunkSizer(session.ceiling).next(largest)), static_cast<unsigned>(largest),
          static_cast<unsigned>(session.ceiling));
  return chunkedGet(http, url, sink, largest, session, largestFreeBlock);
}

}  // namespace crosspoint
