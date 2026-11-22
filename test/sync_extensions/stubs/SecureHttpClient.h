#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace fake {
// Returns the response body; an empty body is sent as HTTP 500.
inline std::function<std::string(const std::string& method, const std::string& url, const std::string& body)> server;
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
    const std::string response = fake::server(method, url_, std::string(reinterpret_cast<const char*>(data), length));
    if (response.empty()) return 500;
    sink(reinterpret_cast<const uint8_t*>(response.data()), response.size());
    return 200;
  }
  bool responseComplete() const { return true; }

 private:
  std::string url_;
};
}  // namespace freeink
