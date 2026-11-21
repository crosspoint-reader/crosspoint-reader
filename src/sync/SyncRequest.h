#pragma once

#include <ArduinoJson.h>
#include <SecureHttpClient.h>

#include <memory>
#include <string>

// Authenticated JSON transport for the CrossPoint Sync /api/v1 extensions.
// One buffer serves requests and responses. C3 borrows it from the framebuffer,
// so C3 callers hold a FrameBufferLoan with rendering locked while this object lives.
class SyncRequest {
 public:
  // One clipping may occupy 24 KiB when its 4 KiB text is JSON-escaped.
  static constexpr size_t BUFFER_SIZE = 32 * 1024;

  SyncRequest();
  ~SyncRequest();
  SyncRequest(const SyncRequest&) = delete;
  SyncRequest& operator=(const SyncRequest&) = delete;

  // False when the buffer, the client, or the heap needed for TLS is missing.
  bool ready() const;

  // True when the endpoint answers with the cursor-based delta protocol (sync_version 2).
  bool supportsDeltaSync(const std::string& url);

  // Identifies the configured account, for sync state that must not carry over to another one.
  static std::string account();

  // Sends a GET, or a PUT of doc when upload is set. On success doc holds the response.
  bool send(const std::string& url, JsonDocument& doc, bool upload = false);

  // For a request written straight into the buffer: write up to BUFFER_SIZE - 1 bytes at body(),
  // then PUT the first length of them. On success response holds the answer.
  char* body() { return buffer; }
  bool sendBody(const std::string& url, size_t length, JsonDocument& response);

  // Shared by the sync code so each file does not build its own JSON reader and writer.
  // serialize returns 0 when doc does not fit in size bytes with its terminator.
  static size_t serialize(const JsonDocument& doc, char* out, size_t size);
  static bool parse(JsonDocument& doc, char* text, size_t length);

 private:
  bool transfer(const std::string& url, const char* method, size_t payloadSize, JsonDocument& doc);

  char* buffer = nullptr;
  std::unique_ptr<freeink::SecureHttpClient> http;
};
