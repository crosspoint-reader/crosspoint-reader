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

 private:
  char* buffer = nullptr;
  std::unique_ptr<freeink::SecureHttpClient> http;
};
