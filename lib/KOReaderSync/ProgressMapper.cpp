#include "ProgressMapper.h"

#include <Logging.h>
#include <HalStorage.h>
#include <Serialization.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

KOReaderPosition ProgressMapper::toKOReader(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  KOReaderPosition result;

  // Calculate page progress within current spine item
  float intraSpineProgress = 0.0f;
  if (pos.totalPages > 0) {
    intraSpineProgress = static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages);
  }

  // Calculate overall book progress (0.0-1.0)
  result.percentage = epub->calculateProgress(pos.spineIndex, intraSpineProgress);

  // Generate XPath with estimated paragraph position based on page
  result.xpath = generateXPath(pos.spineIndex, pos.pageNumber, pos.totalPages);

  // Get chapter info for logging
  const int tocIndex = epub->getTocIndexForSpineIndex(pos.spineIndex);
  const std::string chapterName = (tocIndex >= 0) ? epub->getTocItem(tocIndex).title : "unknown";

  LOG_DBG("ProgressMapper", "CrossPoint -> KOReader: chapter='%s', page=%d/%d -> %.2f%% at %s", chapterName.c_str(),
          pos.pageNumber, pos.totalPages, result.percentage * 100, result.xpath.c_str());

  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos,
                                                int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition result;
  result.spineIndex = 0;
  result.pageNumber = 0;
  result.totalPages = 0;

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return result;
  }

  const int spineCount = epub->getSpineItemsCount();

  // Try to extract spine index from XPath DocFragment.
  // Formats: "#_doc_fragment_N" (0-based) or "/body/DocFragment[N]/..." (1-based)
  int xpathSpineIndex = parseDocFragmentIndex(koPos.xpath);
  bool spineFromXPath = false;
  bool isFragmentStart = false;

  if (xpathSpineIndex >= 0 && xpathSpineIndex < spineCount) {
    result.spineIndex = xpathSpineIndex;
    spineFromXPath = true;
    // If the XPath is just "#_doc_fragment_N" with no sub-element path,
    // the position is at the start of the fragment (page 0).
    isFragmentStart = (koPos.xpath.find('#') != std::string::npos && koPos.xpath.find('/') == std::string::npos);
    LOG_DBG("ProgressMapper", "Spine %d from XPath (start=%s)", xpathSpineIndex, isFragmentStart ? "yes" : "no");
  } else {
    // Fallback: use percentage to find spine item
    const size_t targetBytes = static_cast<size_t>(bookSize * koPos.percentage);
    bool spineFound = false;
    for (int i = 0; i < spineCount; i++) {
      const size_t cumulativeSize = epub->getCumulativeSpineItemSize(i);
      if (cumulativeSize >= targetBytes) {
        result.spineIndex = i;
        spineFound = true;
        break;
      }
    }
    if (!spineFound && spineCount > 0) {
      result.spineIndex = spineCount - 1;
    }
    LOG_DBG("ProgressMapper", "Spine %d from percentage fallback (%.2f%%)", result.spineIndex, koPos.percentage * 100);
  }

  // Try XPath element-based page lookup from the section cache (most precise)
  if (spineFromXPath && !isFragmentStart) {
    auto element = parseXPathElement(koPos.xpath);
    if (element) {
      const std::string sectionPath =
          epub->getCachePath() + "/sections/" + std::to_string(result.spineIndex) + ".bin";
      auto page = lookupElementPage(sectionPath, element->first, element->second);
      if (page) {
        result.pageNumber = *page;
        LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d (element lookup)",
                koPos.percentage * 100, koPos.xpath.c_str(), result.spineIndex, result.pageNumber);
        return result;
      }
    }
  }

  // Estimate page number within the spine item using percentage
  if (result.spineIndex < spineCount) {
    const size_t prevCumSize = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
    const size_t currentCumSize = epub->getCumulativeSpineItemSize(result.spineIndex);
    const size_t spineSize = currentCumSize - prevCumSize;

    int estimatedTotalPages = 0;

    // If we are in the same spine, use the known total pages
    if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
      estimatedTotalPages = totalPagesInCurrentSpine;
    }
    // Otherwise try to estimate based on density from current spine
    else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
      const size_t prevCurrCumSize =
          (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const size_t currCumSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const size_t currSpineSize = currCumSize - prevCurrCumSize;

      if (currSpineSize > 0) {
        float ratio = static_cast<float>(spineSize) / static_cast<float>(currSpineSize);
        estimatedTotalPages = static_cast<int>(totalPagesInCurrentSpine * ratio);
        if (estimatedTotalPages < 1) estimatedTotalPages = 1;
      }
    }

    result.totalPages = estimatedTotalPages;

    // Use percentage to estimate position within the spine item,
    // unless the XPath indicates the start of a fragment (page 0).
    if (!isFragmentStart && spineSize > 0 && estimatedTotalPages > 0) {
      // Calculate what fraction of this spine item the percentage corresponds to
      const float spineStartPct = static_cast<float>(prevCumSize) / static_cast<float>(bookSize);
      const float spineEndPct = static_cast<float>(currentCumSize) / static_cast<float>(bookSize);
      const float spineRange = spineEndPct - spineStartPct;

      float intraSpineProgress = 0.0f;
      if (spineRange > 0.0f) {
        intraSpineProgress = (koPos.percentage - spineStartPct) / spineRange;
        intraSpineProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
      }

      result.pageNumber = static_cast<int>(intraSpineProgress * estimatedTotalPages);
      result.pageNumber = std::max(0, std::min(result.pageNumber, estimatedTotalPages - 1));
    }
  }

  LOG_DBG("ProgressMapper", "KOReader -> CrossPoint: %.2f%% at %s -> spine=%d, page=%d (xpath=%s)", koPos.percentage * 100,
          koPos.xpath.c_str(), result.spineIndex, result.pageNumber, spineFromXPath ? "yes" : "no");

  return result;
}

int ProgressMapper::parseDocFragmentIndex(const std::string& xpath) {
  // KOReader sends progress in two possible formats:
  //   1. XPath: "/body/DocFragment[N]/..." where N is 1-based
  //   2. Anchor: "#_doc_fragment_N" where N is 0-based
  // Returns 0-based spine index, or -1 if parsing fails.

  const char* str = xpath.c_str();

  // Try anchor format first: "#_doc_fragment_N" (0-based)
  const char* anchor = strstr(str, "#_doc_fragment_");
  if (anchor) {
    const char* numStart = anchor + 15;  // strlen("#_doc_fragment_")
    char* numEnd = nullptr;
    long idx = strtol(numStart, &numEnd, 10);
    if (numEnd != numStart && idx >= 0) {
      return static_cast<int>(idx);  // Already 0-based
    }
  }

  // Try XPath format: "/body/DocFragment[N]/" (1-based)
  const char* prefix = "DocFragment[";
  const char* pos = strstr(str, prefix);
  if (pos) {
    const char* numStart = pos + 12;  // strlen("DocFragment[")
    char* numEnd = nullptr;
    long docFragment = strtol(numStart, &numEnd, 10);
    if (numEnd != numStart && *numEnd == ']' && docFragment >= 1) {
      return static_cast<int>(docFragment - 1);  // Convert 1-based to 0-based
    }
  }

  return -1;
}

std::optional<std::pair<ProgressMapper::ElementType, int>> ProgressMapper::parseXPathElement(const std::string& xpath) {
  const char* str = xpath.c_str();

  // Look for "img" followed by ".N" or "[N]" — image index (0-based)
  const char* imgPos = strstr(str, "img");
  if (imgPos) {
    const char* after = imgPos + 3;
    if (*after == '.') {
      char* end = nullptr;
      long idx = strtol(after + 1, &end, 10);
      if (end != after + 1 && idx >= 0) {
        return std::pair{ElementType::IMAGE, static_cast<int>(idx)};
      }
    } else if (*after == '[') {
      char* end = nullptr;
      long idx = strtol(after + 1, &end, 10);
      if (end != after + 1 && *end == ']' && idx >= 0) {
        return std::pair{ElementType::IMAGE, static_cast<int>(idx)};
      }
    }
  }

  // Look for "p[N]" — paragraph index (1-based in XPath, convert to 0-based)
  // Search backwards to find the deepest/last p[N] in the path
  const char* pPos = nullptr;
  const char* search = str;
  while ((search = strstr(search, "/p[")) != nullptr) {
    pPos = search + 1;  // point to 'p'
    search += 3;
  }
  if (pPos && pPos[0] == 'p' && pPos[1] == '[') {
    char* end = nullptr;
    long idx = strtol(pPos + 2, &end, 10);
    if (end != pPos + 2 && *end == ']' && idx >= 1) {
      return std::pair{ElementType::PARAGRAPH, static_cast<int>(idx - 1)};  // 1-based to 0-based
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> ProgressMapper::lookupElementPage(const std::string& sectionFilePath, const ElementType type,
                                                          const int index) {
  if (index < 0) {
    return std::nullopt;
  }

  FsFile f;
  if (!Storage.openFileForRead("PM", sectionFilePath, f)) {
    return std::nullopt;
  }

  // Section cache header: the last 3 uint32_t fields are lutOffset, anchorMapOffset, elementMapOffset.
  // We need to read elementMapOffset (the very last field before page data starts).
  // HEADER_SIZE is defined in Section.cpp but we know the layout ends with 3 × uint32_t.
  // Seek to the version byte first to validate.
  uint8_t version;
  serialization::readPod(f, version);
  if (version < 19) {
    // Element map not available in older cache formats
    f.close();
    return std::nullopt;
  }

  // Seek to end of header to read elementMapOffset (last uint32_t in header)
  const uint32_t fileSize = f.size();
  // Header layout ends with: pageCount(2) + lutOffset(4) + anchorMapOffset(4) + elementMapOffset(4)
  // Total header = version(1)+fontId(4)+lineComp(4)+extraPara(1)+paraAlign(1)+vw(2)+vh(2)+hyph(1)+embed(1)+imgRend(1)
  //              + pageCount(2) + lutOffset(4) + anchorMapOffset(4) + elementMapOffset(4) = 32 bytes
  // elementMapOffset is at offset 28
  constexpr uint32_t ELEMENT_MAP_OFFSET_POS = 28;
  f.seek(ELEMENT_MAP_OFFSET_POS);
  uint32_t elementMapOffset;
  serialization::readPod(f, elementMapOffset);
  if (elementMapOffset == 0 || elementMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(elementMapOffset);

  // Element map format: [paragraphCount(u16)][page...] [imageCount(u16)][page...]
  uint16_t paragraphCount;
  serialization::readPod(f, paragraphCount);

  if (type == ElementType::PARAGRAPH) {
    if (index >= paragraphCount) {
      f.close();
      return std::nullopt;
    }
    f.seek(elementMapOffset + sizeof(uint16_t) + static_cast<uint32_t>(index) * sizeof(uint16_t));
    uint16_t page;
    serialization::readPod(f, page);
    f.close();
    return page;
  }

  // Skip past paragraph data to image data
  const uint32_t imageDataOffset =
      elementMapOffset + sizeof(uint16_t) + static_cast<uint32_t>(paragraphCount) * sizeof(uint16_t);
  f.seek(imageDataOffset);

  uint16_t imageCount;
  serialization::readPod(f, imageCount);

  if (type == ElementType::IMAGE) {
    if (index >= imageCount) {
      f.close();
      return std::nullopt;
    }
    f.seek(imageDataOffset + sizeof(uint16_t) + static_cast<uint32_t>(index) * sizeof(uint16_t));
    uint16_t page;
    serialization::readPod(f, page);
    f.close();
    return page;
  }

  f.close();
  return std::nullopt;
}

std::string ProgressMapper::generateXPath(int spineIndex, int pageNumber, int totalPages) {
  // KOReader uses 1-based DocFragment indices (XPath standard)
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}
