#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace freeink {
inline uint32_t fakeNow = 0;
class SecureHttpClient {
 public:
  using DataCallback = std::function<bool(const uint8_t*, size_t)>;
  using AbortCallback = std::function<bool()>;
  struct Reply {
    int status;
    std::string body;
    uint32_t delayMs;
    uint32_t afterDataMs = 0;
    bool pollAbort = true;
  };
  struct Sent {
    std::string url, method, body;
  };
  static inline std::vector<Reply> replies;
  static inline std::vector<Sent> sent;
  static inline size_t sends = 0;
  static inline uint32_t beginDelayMs = 0;
  static inline bool aborted = false;
  void setUserAgent(const char*) {}
  void setInsecure() {}
  void setTimeout(uint32_t) {}
  bool begin(const std::string& value) {
    url = value;
    fakeNow += beginDelayMs;
    return true;
  }
  void addHeader(const std::string&, const std::string&) {}
  int sendRequest(const char* method, const uint8_t* body, size_t len, const DataCallback& onData,
                  const AbortCallback& shouldAbort = nullptr) {
    aborted = false;
    if (sends >= replies.size()) return -1;
    sent.push_back({url, method, std::string(reinterpret_cast<const char*>(body), len)});
    const Reply r = replies[sends++];
    if (r.pollAbort && shouldAbort && shouldAbort()) {
      aborted = true;
      return -1;
    }
    fakeNow += r.delayMs;
    if (r.pollAbort && shouldAbort && shouldAbort()) {
      aborted = true;
      return -1;
    }
    if (!r.body.empty() && !onData(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size())) return -1;
    fakeNow += r.afterDataMs;
    return r.status;
  }
  bool responseComplete() const { return !aborted; }
  std::vector<std::pair<std::string, std::string>> getHeaders() const { return {}; }

 private:
  std::string url;
};
}  // namespace freeink
