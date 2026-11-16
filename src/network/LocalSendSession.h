#pragma once
#if (defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND) || defined(CROSSPOINT_LOCALSEND_HOST_TEST)
#include <cstddef>
#include <cstdint>

namespace localsend {
constexpr size_t MAX_METADATA = 8192;
constexpr uint32_t MAX_FILE_BYTES = 1024u * 1024u * 1024u;  // 1 GiB per file; streamed to SD.
constexpr size_t MAX_OFFER_FILES = 16;
constexpr uint64_t MAX_TOTAL_BYTES = uint64_t(MAX_FILE_BYTES) * MAX_OFFER_FILES;
constexpr uint32_t SESSION_TIMEOUT_MS = 120000;
constexpr uint32_t CONSENT_TIMEOUT_MS = 60000;
enum class State { Idle, AwaitingConsent, Prepared, Receiving, Failed };
struct OfferedFile {
  char fileId[129]{}, fileName[129]{}, sha256[65]{}, token[33]{};
  uint32_t expected = 0, received = 0;
  bool uploaded = false;
};
struct Session {
  State state = State::Idle;
  OfferedFile files[MAX_OFFER_FILES]{};
  uint8_t fileCount = 0, uploadedCount = 0;
  char sessionId[33]{};
  uint32_t peer = 0, lastActivity = 0;
  int prepare(const char* json, size_t length, uint32_t address, uint32_t now, const char* id, const char* secret,
              const char** rejectionReason = nullptr);
  OfferedFile* authorizedFile(const char* id, const char* file, const char* secret, uint32_t address, uint32_t now);
  bool authorized(const char* id, const char* file, const char* secret, uint32_t address, uint32_t now) const;
  bool cancellable(const char* id, uint32_t address, uint32_t now) const;
  bool expired(uint32_t now) const;
  bool consume(OfferedFile& file, size_t length, uint32_t now);
  bool complete(const OfferedFile& file) const;
  bool finishFile(OfferedFile& file, uint32_t now);
  void reset();
};
enum class Route { Info, Register, Prepare, Upload, Cancel };
struct Request {
  Route route = Route::Info;
  uint32_t length = 0;
  char sessionId[33]{}, fileId[129]{}, token[33]{};
  bool expectContinue = false;
  bool chunked = false;
};
int parseRequest(char* header, Request& request);
bool safeReceiveName(const char* name);
bool validFileSignature(const char* name, const uint8_t* prefix, size_t length);
bool safeDestinationFolder(const char* path);
bool parseChunkSize(const char* text, uint32_t& value);
bool parseLength(const char* text, uint32_t& value);
}  // namespace localsend
#endif
