#include <Logging.h>

#include "network/CrossPointWebServer.h"

// Browsers cannot listen for the device's HTTP/WebDAV connections.
CrossPointWebServer::CrossPointWebServer() = default;
CrossPointWebServer::~CrossPointWebServer() = default;
void CrossPointWebServer::begin() { LOG_ERR("PREVIEW", "File transfer server is unavailable in the browser preview"); }
void CrossPointWebServer::stop() {}
void CrossPointWebServer::handleClient() {}
CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const { return {}; }
