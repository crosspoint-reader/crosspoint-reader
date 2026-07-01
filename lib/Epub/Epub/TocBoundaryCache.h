#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Epub;

class TocBoundaryCache {
 public:
  struct ChapterMetrics {
    int offset = 0;
    int totalPages = 0;
  };

  struct TocEntry {
    int tocIndex;
    std::optional<int32_t> startPageInSpine;  // nullopt = uncomputed, -1 = not found, >= 0 = page
    std::optional<ChapterMetrics> metrics;
  };

  using GetPageForAnchorFn = std::function<std::optional<uint16_t>(const std::string&)>;
  TocBoundaryCache(std::shared_ptr<Epub> epub, int spineIndex, GetPageForAnchorFn getPageForAnchorCallback);

  std::optional<uint16_t> getTocStartPage(int tocIndex);
  int getTocIndexForPage(int pageInSpine);
  TocEntry* getEntry(int tocIndex);

  std::optional<uint16_t> pageCount;

 private:
  std::shared_ptr<Epub> epub;
  int spineIndex;
  GetPageForAnchorFn getPageForAnchorCallback;

  std::optional<int> baseTocIndex;
  std::optional<std::vector<TocEntry>> entries;

  void ensureEntriesLoaded();
};
