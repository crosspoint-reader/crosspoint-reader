#include "SectionHandle.h"

#include <Logging.h>
#include <Memory.h>

#include <utility>

#include "Epub.h"
#include "Page.h"

std::unique_ptr<SectionHandle> SectionHandle::openOrCreate(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                           GfxRenderer& renderer,
                                                           const IncrementalSection::LayoutCacheKey& layoutKey,
                                                           const IncrementalBuildOptions& options) {
  auto handle = makeUniqueNoThrow<SectionHandle>(spineIndex);
  if (!handle) {
    LOG_ERR("SH", "OOM: SectionHandle");
    return nullptr;
  }

  handle->epub_ = epub;
  handle->renderer_ = &renderer;
  handle->layoutKey_ = layoutKey;
  handle->options_ = options;

  const auto sectionsDir = epub->getCachePath() + "/sections";
  handle->incrementalCache_ =
      makeUniqueNoThrow<IncrementalSection::Cache>(sectionsDir, static_cast<uint32_t>(spineIndex));
  if (!handle->incrementalCache_) {
    LOG_ERR("SH", "OOM: IncrementalSection::Cache");
    return handle;
  }

  IncrementalSection::Meta meta;
  if (handle->incrementalCache_->loadMeta(meta) && meta.layoutKey.matches(layoutKey)) {
    if (meta.state == IncrementalSection::CacheState::Complete && handle->incrementalCache_->isComplete()) {
      handle->knownPageCount_ = meta.finalPageCount;
      handle->mode_ = SectionHandleMode::IncrementalComplete;
      return handle;
    }
    handle->incrementalCache_->removeAllFiles();
  }

  handle->startBuilder();
  return handle;
}

bool SectionHandle::startBuilder() {
  if (builder_) {
    return true;
  }
  if (!epub_ || !renderer_) {
    mode_ = SectionHandleMode::MissingOrFailed;
    return false;
  }

  builder_ = makeUniqueNoThrow<IncrementalSectionBuilder>(epub_, spineIndex_, *renderer_, layoutKey_, options_);
  if (!builder_) {
    LOG_ERR("SH", "OOM: IncrementalSectionBuilder");
    mode_ = SectionHandleMode::MissingOrFailed;
    return false;
  }
  if (!builder_->start()) {
    builder_.reset();
    knownPageCount_ = 0;
    mode_ = SectionHandleMode::MissingOrFailed;
    return false;
  }
  knownPageCount_ = builder_->knownPageCount();
  mode_ = SectionHandleMode::IncrementalBuilding;
  return true;
}

bool SectionHandle::hasPage(const uint32_t pageNumber) const {
  if (mode_ == SectionHandleMode::MissingOrFailed) {
    return false;
  }
  return pageNumber < knownPageCount_;
}

bool SectionHandle::ensurePageAvailable(const uint32_t pageNumber, const IncrementalBuildBudget& budget) {
  while (!hasPage(pageNumber) && mode_ == SectionHandleMode::IncrementalBuilding) {
    const auto result = pump(budget);
    if (result.status == SectionPumpStatus::DeferredLowMemory || result.status == SectionPumpStatus::Failed ||
        result.status == SectionPumpStatus::NoWork) {
      return false;
    }
  }
  return hasPage(pageNumber);
}

std::unique_ptr<Page> SectionHandle::loadPage(const uint32_t pageNumber) const {
  if (mode_ == SectionHandleMode::MissingOrFailed) {
    return nullptr;
  }
  if (builder_) {
    return builder_->loadPage(pageNumber);
  }
  return incrementalCache_ ? incrementalCache_->loadPage(pageNumber) : nullptr;
}

bool SectionHandle::isComplete() const { return mode_ == SectionHandleMode::IncrementalComplete; }

uint32_t SectionHandle::knownPageCount() const {
  if (mode_ == SectionHandleMode::MissingOrFailed) {
    return 0;
  }
  return knownPageCount_;
}

uint32_t SectionHandle::finalPageCount() const { return isComplete() ? knownPageCount() : 0; }

SectionPumpResult SectionHandle::pump(const IncrementalBuildBudget& budget) {
  if (mode_ != SectionHandleMode::IncrementalBuilding) {
    return {SectionPumpStatus::NoWork, knownPageCount(), knownPageCount()};
  }
  if (!builder_ && !startBuilder()) {
    return {SectionPumpStatus::Failed, 0, 0};
  }

  const uint32_t pagesBefore = knownPageCount_;
  const auto state = builder_->pump(budget);
  const uint32_t pagesAfter = builder_ ? builder_->knownPageCount() : knownPageCount_;
  return applyPumpState(state, pagesBefore, pagesAfter);
}

SectionPumpResult SectionHandle::applyPumpState(const IncrementalBuildState state, const uint32_t pagesBefore,
                                                const uint32_t pagesAfter) {
  if (state == IncrementalBuildState::DeferredLowMemory) {
    knownPageCount_ = pagesAfter;
    return {SectionPumpStatus::DeferredLowMemory, pagesBefore, pagesAfter};
  }
  if (state == IncrementalBuildState::Complete) {
    knownPageCount_ = pagesAfter;
    mode_ = SectionHandleMode::IncrementalComplete;
    builder_.reset();
    return {SectionPumpStatus::Complete, pagesBefore, pagesAfter};
  }
  if (state == IncrementalBuildState::Failed || state == IncrementalBuildState::Cancelled) {
    knownPageCount_ = 0;
    mode_ = SectionHandleMode::MissingOrFailed;
    builder_.reset();
    return {SectionPumpStatus::Failed, pagesBefore, pagesAfter};
  }
  knownPageCount_ = pagesAfter;
  if (state == IncrementalBuildState::Parsing) {
    return {SectionPumpStatus::Pumped, pagesBefore, pagesAfter};
  }
  if (pagesAfter > pagesBefore) {
    return {SectionPumpStatus::Pumped, pagesBefore, pagesAfter};
  }
  return {SectionPumpStatus::NoWork, pagesBefore, pagesAfter};
}

bool SectionHandle::hasActiveBuilder() const { return builder_ && mode_ == SectionHandleMode::IncrementalBuilding; }

void SectionHandle::cancel() {
  if (builder_) {
    builder_->cancel();
    builder_.reset();
  }
  if (mode_ == SectionHandleMode::IncrementalBuilding) {
    knownPageCount_ = 0;
    mode_ = SectionHandleMode::MissingOrFailed;
  }
}

void SectionHandle::clearCache() {
  cancel();
  if (incrementalCache_) {
    incrementalCache_->removeAllFiles();
  }
  knownPageCount_ = 0;
  mode_ = SectionHandleMode::MissingOrFailed;
}

std::optional<uint16_t> SectionHandle::getPageForAnchor(const std::string& anchor) const {
  if (mode_ == SectionHandleMode::MissingOrFailed || !incrementalCache_) {
    return std::nullopt;
  }
  return incrementalCache_->getPageForAnchor(anchor);
}

std::optional<uint16_t> SectionHandle::getPageForParagraphIndex(const uint16_t paragraphIndex) const {
  if (mode_ == SectionHandleMode::MissingOrFailed || !incrementalCache_) {
    return std::nullopt;
  }
  return incrementalCache_->getPageForParagraphIndex(paragraphIndex);
}

std::optional<uint16_t> SectionHandle::getPageForListItemIndex(const uint16_t listItemIndex) const {
  if (mode_ == SectionHandleMode::MissingOrFailed || !incrementalCache_) {
    return std::nullopt;
  }
  return incrementalCache_->getPageForListItemIndex(listItemIndex);
}

std::optional<uint16_t> SectionHandle::getParagraphIndexForPage(const uint16_t page) const {
  if (mode_ == SectionHandleMode::MissingOrFailed || !incrementalCache_) {
    return std::nullopt;
  }
  return incrementalCache_->getParagraphIndexForPage(page);
}
