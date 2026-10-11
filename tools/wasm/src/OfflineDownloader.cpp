#include <Logging.h>

#include "network/HttpDownloader.h"

bool HttpDownloader::fetchUrl(const std::string&, Stream&, const std::string&, const std::string&) {
  LOG_ERR("PREVIEW", "Network downloads are unavailable in the browser preview");
  return false;
}

bool HttpDownloader::fetchUrl(const std::string&, const DataCallback&, const std::string&, const std::string&) {
  LOG_ERR("PREVIEW", "Network downloads are unavailable in the browser preview");
  return false;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string&, const std::string&, ProgressCallback,
                                                             const bool* cancelFlag, const std::string&,
                                                             const std::string&, const std::vector<Header>&, bool) {
  LOG_ERR("PREVIEW", "Network downloads are unavailable in the browser preview");
  return cancelFlag && *cancelFlag ? ABORTED : HTTP_ERROR;
}
