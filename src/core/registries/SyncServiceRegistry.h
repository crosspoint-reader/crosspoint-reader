#pragma once

#include <cstddef>
#include <cstdint>

// SyncServiceRegistry exposes typed service access to optional sync-provider features
// without leaking feature-specific headers into app-shell code.
//
// Design rationale:
//   - Named slots (one per provider) rather than a generic collection: the set of sync
//     providers is small and stable; a typed slot gives callers a clear, narrow API.
//   - Char-buffer fn ptrs rather than std::string returns: keeps this header free of
//     heap-allocating types so it is safe to include in any translation unit.
//   - Nullable: all fn ptrs default to nullptr; getKoreaderApi() returns nullptr when
//     the feature is disabled, letting callers guard with a single null-check.
//
// Buffer sizes for credential strings.  Callers must allocate at least these sizes.
static constexpr size_t kSyncCredBufSize = 128;  // username / password
static constexpr size_t kSyncUrlBufSize = 256;   // server URL

namespace core {

// Narrow API surface for KOReader sync credential and state access.
// All string accessors write into caller-provided buffers (null-terminated).
struct KoreaderServiceApi {
  bool (*hasCredentials)();

  // Getters: write into outBuf[bufSize].  outBuf is always null-terminated on return.
  void (*getUsername)(char* outBuf, size_t bufSize);
  void (*getPassword)(char* outBuf, size_t bufSize);
  void (*getServerUrl)(char* outBuf, size_t bufSize);
  uint8_t (*getMatchMethod)();

  // Setters: value is null-terminated C string.  save=true flushes to SPIFFS.
  void (*setUsername)(const char* value, bool save);
  void (*setPassword)(const char* value, bool save);
  void (*setServerUrl)(const char* value, bool save);
  void (*setMatchMethod)(uint8_t method, bool save);

  void (*saveSettings)();
};

class SyncServiceRegistry {
 public:
  // Called from KOReaderSync feature registration unit under #if ENABLE_KOREADER_SYNC.
  static void setKoreaderApi(const KoreaderServiceApi& api) {
    koreaderApi = api;
    koreaderRegistered = true;
  }

  // Returns nullptr when ENABLE_KOREADER_SYNC is off (feature not registered).
  static const KoreaderServiceApi* getKoreaderApi() { return koreaderRegistered ? &koreaderApi : nullptr; }

 private:
  inline static bool koreaderRegistered = false;
  inline static KoreaderServiceApi koreaderApi = {};
};

}  // namespace core
