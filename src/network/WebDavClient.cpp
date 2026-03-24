#include "WebDavClient.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <base64.h>

#include <cstring>
#include <memory>

#include "util/UrlUtils.h"

static constexpr const char* PROPFIND_BODY =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\">"
    "<d:prop>"
    "<d:displayname/>"
    "<d:getcontentlength/>"
    "<d:resourcetype/>"
    "</d:prop>"
    "</d:propfind>";

// WebDavParser

WebDavParser::WebDavParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccurred = true;
    LOG_ERR("DAV", "Failed to create XML parser");
  }
}

WebDavParser::~WebDavParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);
    XML_SetElementHandler(parser, nullptr, nullptr);
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

size_t WebDavParser::write(uint8_t c) { return write(&c, 1); }

size_t WebDavParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccurred) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    void* const buf = XML_GetBuffer(parser, chunkSize);
    if (!buf) {
      errorOccurred = true;
      LOG_ERR("DAV", "XML buffer alloc failed");
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }

    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccurred = true;
      LOG_ERR("DAV", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_ParserFree(parser);
      parser = nullptr;
      return length;
    }

    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void WebDavParser::flush() {
  if (parser && XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccurred = true;
    XML_ParserFree(parser);
    parser = nullptr;
  }
}

bool WebDavParser::endsWith(const char* str, const char* suffix) {
  const size_t strLen = strlen(str);
  const size_t suffixLen = strlen(suffix);
  if (suffixLen > strLen) return false;
  return strcmp(str + strLen - suffixLen, suffix) == 0;
}

void XMLCALL WebDavParser::startElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* self = static_cast<WebDavParser*>(userData);

  if (endsWith(name, "response")) {
    self->inResponse = true;
    self->currentEntry = WebDavEntry{};
    self->inCollection = false;
    return;
  }

  if (!self->inResponse) return;

  if (endsWith(name, "href")) {
    self->inHref = true;
    self->currentText.clear();
  } else if (endsWith(name, "displayname")) {
    self->inDisplayName = true;
    self->currentText.clear();
  } else if (endsWith(name, "getcontentlength")) {
    self->inContentLength = true;
    self->currentText.clear();
  } else if (endsWith(name, "collection")) {
    self->inCollection = true;
  }
}

void XMLCALL WebDavParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<WebDavParser*>(userData);

  if (endsWith(name, "response")) {
    if (self->inResponse) {
      self->currentEntry.isCollection = self->inCollection;
      if (!self->currentEntry.href.empty()) {
        self->entries.push_back(std::move(self->currentEntry));
      }
    }
    self->inResponse = false;
    self->inCollection = false;
    return;
  }

  if (!self->inResponse) return;

  if (endsWith(name, "href")) {
    if (self->inHref) {
      self->currentEntry.href = self->currentText;
      if (self->currentEntry.name.empty()) {
        // Extract filename from href as fallback display name
        auto pos = self->currentText.rfind('/');
        if (pos != std::string::npos && pos + 1 < self->currentText.size()) {
          self->currentEntry.name = self->currentText.substr(pos + 1);
        } else if (pos == self->currentText.size() - 1 && self->currentText.size() > 1) {
          auto pos2 = self->currentText.rfind('/', pos - 1);
          if (pos2 != std::string::npos) {
            self->currentEntry.name = self->currentText.substr(pos2 + 1, pos - pos2 - 1);
          }
        }
      }
    }
    self->inHref = false;
  } else if (endsWith(name, "displayname")) {
    if (self->inDisplayName && !self->currentText.empty()) {
      self->currentEntry.name = self->currentText;
    }
    self->inDisplayName = false;
  } else if (endsWith(name, "getcontentlength")) {
    if (self->inContentLength) {
      self->currentEntry.contentLength = strtoul(self->currentText.c_str(), nullptr, 10);
    }
    self->inContentLength = false;
  }
}

void XMLCALL WebDavParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<WebDavParser*>(userData);
  if (self->inHref || self->inDisplayName || self->inContentLength) {
    self->currentText.append(s, len);
  }
}

// WebDavClient

bool WebDavClient::listFiles(const char* url, const char* username, const char* password,
                             std::vector<WebDavEntry>& entries) {
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }

  HTTPClient http;
  http.begin(*client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  http.addHeader("Depth", "1");
  http.addHeader("Content-Type", "application/xml");

  if (username && password && strlen(username) > 0 && strlen(password) > 0) {
    std::string credentials = std::string(username) + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("DAV", "PROPFIND: %s", url);

  const int httpCode = http.sendRequest("PROPFIND", PROPFIND_BODY);

  // WebDAV PROPFIND returns 207 Multi-Status on success
  if (httpCode != 207) {
    LOG_ERR("DAV", "PROPFIND failed: %d", httpCode);
    http.end();
    return false;
  }

  WebDavParser parser;
  {
    WebDavParserStream stream{parser};
    http.writeToStream(&stream);
  }

  http.end();

  if (parser.error()) {
    LOG_ERR("DAV", "Failed to parse PROPFIND response");
    return false;
  }

  auto parsed = std::move(parser).getEntries();
  LOG_DBG("DAV", "Got %d entries", parsed.size());

  // Skip first entry (self/parent directory)
  bool first = true;
  entries.clear();
  entries.reserve(parsed.size());
  for (auto& entry : parsed) {
    if (first) {
      first = false;
      continue;
    }
    entries.push_back(std::move(entry));
  }

  return true;
}
