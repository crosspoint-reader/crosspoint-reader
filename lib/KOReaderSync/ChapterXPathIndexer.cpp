#include "ChapterXPathIndexer.h"

#include <Logging.h>

#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>

#include "ChapterXPathForwardMapper.h"
#include "ChapterXPathIndexerInternal.h"
#include "ChapterXPathReverseMapper.h"

using namespace ChapterXPathIndexerInternal;

// Public facade used by ProgressMapper. It intentionally stays thin and delegates
// heavy parsing/mapping work to the internal forward/reverse modules.

std::string ChapterXPathIndexer::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                      const float intraSpineProgress) {
  return findXPathForProgressInternal(epub, spineIndex, intraSpineProgress);
}

bool ChapterXPathIndexer::findProgressForXPath(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                               const std::string& xpath, float& outIntraSpineProgress,
                                               bool& outExactMatch) {
  return findProgressForXPathInternal(epub, spineIndex, xpath, outIntraSpineProgress, outExactMatch);
}

bool ChapterXPathIndexer::tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex) {
  outSpineIndex = -1;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = normalizeXPath(xpath);
  const std::string key = "/docfragment[";
  const size_t pos = normalized.find(key);
  if (pos == std::string::npos) {
    LOG_DBG("KOX", "No DocFragment in xpath: '%s'", xpath.c_str());
    return false;
  }

  const size_t start = pos + key.size();
  size_t end = start;
  while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
    end++;
  }

  if (end == start || end >= normalized.size() || normalized[end] != ']') {
    return false;
  }

  const std::string value = normalized.substr(start, end - start);
  const long parsed = std::strtol(value.c_str(), nullptr, 10);
  // XPath uses 1-based predicates; internal spine indexing is 0-based.
  if (parsed < 1 || parsed > std::numeric_limits<int>::max()) {
    return false;
  }

  outSpineIndex = static_cast<int>(parsed) - 1;
  return true;
}

bool ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(const std::string& xpath, uint16_t& outParagraphIndex) {
  outParagraphIndex = 0;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = normalizeXPath(xpath);

  const std::string bodyKey = "/body";
  size_t secondBody = normalized.find(bodyKey);
  if (secondBody != std::string::npos) {
    secondBody = normalized.find(bodyKey, secondBody + bodyKey.size());
  }

  const std::string pKey = "/p[";
  const size_t pos = normalized.find(pKey, secondBody != std::string::npos ? secondBody : 0);
  if (pos == std::string::npos) {
    return false;
  }

  const size_t start = pos + pKey.size();
  size_t end = start;
  while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
    end++;
  }

  if (end == start || end >= normalized.size() || normalized[end] != ']') {
    return false;
  }

  const long parsed = std::strtol(normalized.substr(start, end - start).c_str(), nullptr, 10);
  // Paragraph index is preserved as 1-based to match XPath p[N] convention.
  if (parsed < 1 || parsed > UINT16_MAX) {
    return false;
  }

  outParagraphIndex = static_cast<uint16_t>(parsed);
  return true;
}
