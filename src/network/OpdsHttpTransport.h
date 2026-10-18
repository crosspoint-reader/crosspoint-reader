#pragma once
#include <OpdsTransport.h>

/**
 * Firmware implementation of the SDK OpdsClient's transport: streaming GET and
 * form POST over HttpDownloader (esp_http_client + wolfSSL), and URL joining
 * through UrlUtils. Stateless — one instance is shared by an OpdsClient.
 */
class OpdsHttpTransport final : public freeink::opds::OpdsTransport {
 public:
  freeink::opds::HttpResult get(const std::string& url, const freeink::opds::HttpAuth& auth, const char* accept,
                                const char* acceptLanguage, bool captureErrorBody,
                                const freeink::opds::OpdsDataSink& onChunk) override;

  bool postForm(const std::string& url, const std::string& body, std::string& outResponse, int& outStatus) override;

  std::string resolveUrl(const std::string& base, const std::string& ref) override;
};
