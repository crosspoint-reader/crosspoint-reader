#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "BuildScratch.h"

namespace fake {
inline std::vector<std::string> uploads;
inline std::string remoteResponse;
inline int requests = 0;
inline int failHttpAt = -1;
inline bool incomplete = false;
}  // namespace fake
namespace freeink {
class SecureHttpClient {
 public:
  void setInsecure() {}
  bool begin(const std::string& url) {
    url_ = url;
    return true;
  }
  void addHeader(const std::string&, const std::string&) {}
  template <typename Sink>
  int sendRequest(const char* method, const uint8_t* data, size_t length, Sink sink) {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
    // The transfer must hold the exclusive scratch claim until its response is parsed.
    assert(buildscratch::claim(32768) == nullptr);
    assert(!fake::syncBufferAllocated);
#endif
    if (fake::requests++ == fake::failHttpAt) return 500;
    const bool upload = std::string(method) == "PUT";
    if (upload) fake::uploads.emplace_back(reinterpret_cast<const char*>(data), length);
    const std::string response = upload                                        ? "{\"accepted\":1}"
                                 : url_.find("?cursor=0") != std::string::npos ? "{\"sync_version\":2}"
                                                                               : fake::remoteResponse;
    complete_ = !fake::incomplete;
    for (size_t offset = 0; offset < response.size(); offset += 97) {
      const size_t chunk = std::min<size_t>(97, response.size() - offset);
      if (!sink(reinterpret_cast<const uint8_t*>(response.data() + offset), chunk)) {
        complete_ = false;
        break;
      }
    }
    return 200;
  }
  bool responseComplete() const { return complete_; }

 private:
  std::string url_;
  bool complete_ = false;
};
}  // namespace freeink
