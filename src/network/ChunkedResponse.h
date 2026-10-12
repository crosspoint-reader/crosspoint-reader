#pragma once

#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>

#include <cstring>
#include <memory>

// Coalesces a chunked (CONTENT_LENGTH_UNKNOWN) response body into TCP-sized
// writes. Written naively, a streamed listing calls sendContent() once per
// entry, and every call is its own HTTP chunk — chunk header, TCP write, and
// the WebServer library's internal malloc for the chunk-size line. On a folder
// with a few hundred files that dominates the response time.
//
// Ported from CrossInk commit 1d6b82f ("perf: batch web file list responses",
// Julia Nguyen and Justin Mitchell), which back-ported crosspoint-reader
// 302c0771. Generalised here from the JSON file listing to every streamed list
// response, including WebDAV PROPFIND.
class ChunkedResponse {
 public:
  explicit ChunkedResponse(WebServer* server) : server(server) {
    // ~1 TCP segment. Heap rather than stack: handleClient() runs on the shared
    // Arduino loop task, below the whole activity call chain, so 1.4KB is not
    // free there.
    // The allocation is fallible and a failure degrades to the old per-entry
    // sends rather than failing the request.
    buffer = makeUniqueNoThrow<char[]>(CAPACITY);
    if (!buffer) {
      LOG_ERR("WEB", "OOM: chunked response buffer; falling back to per-entry sends");
    }
  }

  void append(const char* data, size_t len) {
    if (!buffer) {
      server->sendContent(data, len);
      return;
    }
    if (used + len > CAPACITY) flush();
    if (len > CAPACITY) {
      // Longer than the whole buffer — send it on its own rather than truncating.
      server->sendContent(data, len);
      return;
    }
    // Typed local rather than arithmetic directly on get(): cppcheck does not
    // resolve the unique_ptr<char[]> specialisation and reads get() as void*.
    char* const base = buffer.get();
    memcpy(base + used, data, len);
    used += len;
  }

  void append(const char* str) { append(str, strlen(str)); }

  // Flushes the tail and terminates the chunked response with the empty chunk.
  void finish() {
    flush();
    server->sendContent("");
  }

 private:
  static constexpr size_t CAPACITY = 1400;

  void flush() {
    if (used == 0) return;
    HalSystem::feedWatchdog();
    server->sendContent(buffer.get(), used);
    used = 0;
    // Yield so WiFi and the other tasks get to run: sendContent() is a blocking
    // network write that can stall for seconds under concurrent connections.
    yield();
    HalSystem::feedWatchdog();
  }

  WebServer* server;
  std::unique_ptr<char[]> buffer;
  size_t used = 0;
};

// Print on top of ChunkedResponse, for writers that speak Print (the reading-stats dashboard and export).
class ChunkedPrint final : public Print {
 public:
  explicit ChunkedPrint(ChunkedResponse& out) : out(out) {}

  size_t write(uint8_t b) override {
    const char c = static_cast<char>(b);
    out.append(&c, 1);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    out.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

 private:
  ChunkedResponse& out;
};

// JSON-array framing on top of ChunkedResponse: owns the brackets and the
// separating commas so callers cannot emit a leading or doubled comma when an
// entry is skipped.
class ChunkedJsonArray {
 public:
  explicit ChunkedJsonArray(WebServer* server) : out(server) { out.append("[", 1); }

  void addEntry(const char* json, size_t len) {
    if (seenFirst) out.append(",", 1);
    seenFirst = true;
    out.append(json, len);
  }

  // The next entry, to be written through Print -- serializeJson(doc, array.nextEntry()) --
  // with no buffer of its own and so no size limit.
  Print& nextEntry() {
    if (seenFirst) out.append(",", 1);
    seenFirst = true;
    return print;
  }

  void finish() {
    out.append("]", 1);
    out.finish();
  }

 private:
  ChunkedResponse out;
  ChunkedPrint print{out};
  bool seenFirst = false;
};
