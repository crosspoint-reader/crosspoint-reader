#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "IncrementalSectionBuilder.h"

class Epub;
class GfxRenderer;
class Page;

enum class SectionHandleMode {
  IncrementalComplete,
  IncrementalBuilding,
  MissingOrFailed,
};

enum class SectionPumpStatus {
  NoWork,
  Pumped,
  DeferredLowMemory,
  Complete,
  Failed,
};

struct SectionPumpResult {
  SectionPumpStatus status = SectionPumpStatus::NoWork;
  uint32_t pagesBefore = 0;
  uint32_t pagesAfter = 0;
};

class SectionHandle {
 public:
  explicit SectionHandle(int spineIndex) : spineIndex_(spineIndex) {}

  static std::unique_ptr<SectionHandle> openOrCreate(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                     GfxRenderer& renderer,
                                                     const IncrementalSection::LayoutCacheKey& layoutKey,
                                                     const IncrementalBuildOptions& options);

  int currentPage = 0;

  bool hasPage(uint32_t pageNumber) const;
  bool ensurePageAvailable(uint32_t pageNumber, const IncrementalBuildBudget& budget);
  std::unique_ptr<Page> loadPage(uint32_t pageNumber) const;
  bool isComplete() const;
  uint32_t knownPageCount() const;
  uint32_t finalPageCount() const;
  SectionPumpResult pump(const IncrementalBuildBudget& budget);
  bool hasActiveBuilder() const;
  void cancel();
  void clearCache();
  SectionHandleMode mode() const { return mode_; }
  int spineIndex() const { return spineIndex_; }

  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t paragraphIndex) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t listItemIndex) const;
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

 private:
  bool startBuilder();
  SectionPumpResult applyPumpState(IncrementalBuildState state, uint32_t pagesBefore, uint32_t pagesAfter);

  int spineIndex_;
  SectionHandleMode mode_ = SectionHandleMode::MissingOrFailed;
  std::unique_ptr<IncrementalSection::Cache> incrementalCache_;
  std::unique_ptr<IncrementalSectionBuilder> builder_;
  std::shared_ptr<Epub> epub_;
  GfxRenderer* renderer_ = nullptr;
  IncrementalSection::LayoutCacheKey layoutKey_{};
  IncrementalBuildOptions options_{};
  uint32_t knownPageCount_ = 0;
};
