#include "OpdsHttpTransport.h"

#include "network/HttpDownloader.h"
#include "util/UrlUtils.h"

namespace fo = freeink::opds;

fo::HttpResult OpdsHttpTransport::get(const std::string& url, const fo::HttpAuth& auth, const char* accept,
                                      const char* acceptLanguage, const bool captureErrorBody,
                                      const fo::OpdsDataSink& onChunk) {
  fo::HttpResult result;
  HttpDownloader::FetchOptions options;
  options.username = auth.username;
  options.password = auth.password;
  options.bearer = auth.bearer;
  options.headers = auth.headers;
  if (accept) options.accept = accept;
  if (acceptLanguage) options.acceptLanguage = acceptLanguage;
  options.captureErrorBody = captureErrorBody;
  options.statusOut = &result.status;
  if (captureErrorBody) options.wwwAuthenticateOut = &result.wwwAuthenticate;
  result.ok = HttpDownloader::fetchUrl(url, onChunk, options);
  result.redirected = HttpDownloader::lastAnswerRedirected();
  return result;
}

bool OpdsHttpTransport::postForm(const std::string& url, const std::string& body, std::string& outResponse,
                                 int& outStatus) {
  return HttpDownloader::postForm(url, body, outResponse, &outStatus);
}

std::string OpdsHttpTransport::resolveUrl(const std::string& base, const std::string& ref) {
  return UrlUtils::buildUrl(base, ref);
}
