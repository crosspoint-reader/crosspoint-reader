#include "OpdsFeedParser.h"

#include <Logging.h>
#include <Memory.h>

#include <cctype>

namespace {
const std::string kNoValue;
constexpr uint8_t UTF8_BOM[3] = {0xEF, 0xBB, 0xBF};
}  // namespace

bool OpdsFeedParser::selectBackend(const uint8_t firstByte) {
  if (firstByte == '{') {
    jsonParser = makeUniqueNoThrow<Opds2Parser>();
    if (!jsonParser) {
      LOG_ERR("OPDS", "OOM: OPDS 2.0 parser");
      allocFailed = true;
      return false;
    }
    LOG_DBG("OPDS", "Feed detected as OPDS 2.0 (JSON)");
  } else {
    xmlParser = makeUniqueNoThrow<OpdsParser>();
    if (!xmlParser) {
      LOG_ERR("OPDS", "OOM: OPDS 1.x parser");
      allocFailed = true;
      return false;
    }
    LOG_DBG("OPDS", "Feed detected as OPDS 1.x (Atom)");
  }
  return true;
}

void OpdsFeedParser::reset() {
  xmlParser.reset();
  jsonParser.reset();
  bomBytesSkipped = 0;
  allocFailed = false;
}

size_t OpdsFeedParser::write(const uint8_t c) { return write(&c, 1); }

size_t OpdsFeedParser::write(const uint8_t* data, size_t length) {
  if (allocFailed) return length;
  const size_t consumed = length;

  if (!xmlParser && !jsonParser) {
    // Skip an optional UTF-8 BOM and leading whitespace, then sniff.
    while (length > 0) {
      if (bomBytesSkipped < sizeof(UTF8_BOM) && *data == UTF8_BOM[bomBytesSkipped]) {
        ++bomBytesSkipped;
        ++data;
        --length;
        continue;
      }
      if (isspace(*data)) {
        ++data;
        --length;
        continue;
      }
      break;
    }
    if (length == 0) return consumed;
    if (!selectBackend(*data)) return consumed;
  }

  if (jsonParser) {
    jsonParser->write(data, length);
  } else {
    xmlParser->write(data, length);
  }
  return consumed;
}

void OpdsFeedParser::flush() {
  if (jsonParser) jsonParser->flush();
  if (xmlParser) xmlParser->flush();
}

bool OpdsFeedParser::error() const {
  if (allocFailed) return true;
  if (jsonParser) return jsonParser->error();
  if (xmlParser) return xmlParser->error();
  return true;  // empty body: nothing was parsed
}

bool OpdsFeedParser::truncated() const {
  if (jsonParser) return jsonParser->truncated();
  if (xmlParser) return xmlParser->truncated();
  return false;
}

std::vector<OpdsEntry> OpdsFeedParser::takeEntries() {
  if (jsonParser) return std::move(*jsonParser).getEntries();
  if (xmlParser) return std::move(*xmlParser).getEntries();
  return {};
}

std::vector<OpdsEntry> OpdsFeedParser::takeFacetEntries() {
  if (jsonParser) return jsonParser->takeFacetEntries();
  return {};
}

const std::string& OpdsFeedParser::getFeedTitle() const {
  if (jsonParser) return jsonParser->getFeedTitle();
  if (xmlParser) return xmlParser->getFeedTitle();
  return kNoValue;
}

const std::string& OpdsFeedParser::getSearchTemplate() const {
  if (jsonParser) return jsonParser->getSearchTemplate();
  if (xmlParser) return xmlParser->getSearchTemplate();
  return kNoValue;
}

const std::string& OpdsFeedParser::getSearchDescriptionUrl() const {
  if (xmlParser) return xmlParser->getSearchDescriptionUrl();
  return kNoValue;  // OPDS 2.0 search is always an inline URI template
}

const std::string& OpdsFeedParser::getNextPageUrl() const {
  if (jsonParser) return jsonParser->getNextPageUrl();
  if (xmlParser) return xmlParser->getNextPageUrl();
  return kNoValue;
}

const std::string& OpdsFeedParser::getPrevPageUrl() const {
  if (jsonParser) return jsonParser->getPrevPageUrl();
  if (xmlParser) return xmlParser->getPrevPageUrl();
  return kNoValue;
}

const std::string& OpdsFeedParser::getFirstPageUrl() const {
  if (jsonParser) return jsonParser->getFirstPageUrl();
  if (xmlParser) return xmlParser->getFirstPageUrl();
  return kNoValue;
}

const std::string& OpdsFeedParser::getLastPageUrl() const {
  if (jsonParser) return jsonParser->getLastPageUrl();
  if (xmlParser) return xmlParser->getLastPageUrl();
  return kNoValue;
}

uint32_t OpdsFeedParser::getNumberOfItems() const {
  if (jsonParser) return jsonParser->getNumberOfItems();
  if (xmlParser) return xmlParser->getNumberOfItems();
  return 0;
}

uint32_t OpdsFeedParser::getItemsPerPage() const {
  if (jsonParser) return jsonParser->getItemsPerPage();
  if (xmlParser) return xmlParser->getItemsPerPage();
  return 0;
}

uint32_t OpdsFeedParser::getCurrentPage() const {
  if (jsonParser) return jsonParser->getCurrentPage();
  if (xmlParser) return xmlParser->getCurrentPage();
  return 0;
}
