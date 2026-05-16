#include "IncrementalSectionBuilder.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <utility>

#include "Epub.h"
#include "Page.h"
#include "css/CssParser.h"
#include "hyphenation/Hyphenator.h"

namespace {

constexpr uint32_t LOW_HEAP_LOG_INTERVAL_MS = 1000;

const char* parsePumpStatusName(const ParsePumpStatus status) {
  switch (status) {
    case ParsePumpStatus::NeedsMore:
      return "NeedsMore";
    case ParsePumpStatus::PageReady:
      return "PageReady";
    case ParsePumpStatus::Complete:
      return "Complete";
    case ParsePumpStatus::Cancelled:
      return "Cancelled";
    case ParsePumpStatus::Error:
      return "Error";
  }
  return "Unknown";
}

}  // namespace

IncrementalSectionBuilder::IncrementalSectionBuilder(std::shared_ptr<Epub> epub, const int spineIndex,
                                                     GfxRenderer& renderer,
                                                     IncrementalSection::LayoutCacheKey layoutKey,
                                                     IncrementalBuildOptions options)
    : epub_(std::move(epub)),
      spineIndex_(spineIndex),
      renderer_(renderer),
      layoutKey_(layoutKey),
      options_(std::move(options)),
      cache_(epub_->getCachePath() + "/sections", static_cast<uint32_t>(spineIndex)) {}

IncrementalSectionBuilder::~IncrementalSectionBuilder() { cancel(); }

bool IncrementalSectionBuilder::extractTempHtml(uint32_t& fileSize) {
  const auto localPath = epub_->getSpineItem(spineIndex_).href;
  tmpHtmlPath_ = epub_->getCachePath() + "/.tmp_inc_" + std::to_string(spineIndex_) + ".html";

  bool success = false;
  fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("ISB", "Retrying incremental stream (attempt %d)", attempt + 1);
      delay(50);
    }

    if (Storage.exists(tmpHtmlPath_.c_str())) {
      Storage.remove(tmpHtmlPath_.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("ISB", tmpHtmlPath_, tmpHtml)) {
      continue;
    }
    success = epub_->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    if (!success && Storage.exists(tmpHtmlPath_.c_str())) {
      Storage.remove(tmpHtmlPath_.c_str());
    }
  }

  if (!success) {
    LOG_ERR("ISB", "Failed to extract temp HTML for spine %d", spineIndex_);
  }
  return success;
}

void IncrementalSectionBuilder::cleanupTempHtml() const {
  if (!tmpHtmlPath_.empty() && Storage.exists(tmpHtmlPath_.c_str())) {
    Storage.remove(tmpHtmlPath_.c_str());
  }
}

bool IncrementalSectionBuilder::start() {
  if (state_ != IncrementalBuildState::Idle) {
    return state_ == IncrementalBuildState::Parsing || state_ == IncrementalBuildState::Complete;
  }

  state_ = IncrementalBuildState::Extracting;
  cancelRequested_ = false;
  appendFailed_ = false;
  knownPageCount_ = 0;
  generationId_ = millis();

  uint32_t fileSize = 0;
  if (!extractTempHtml(fileSize)) {
    failAndCleanup();
    return false;
  }
  sourceFileSize_ = fileSize;

  if (!cache_.beginBuild(layoutKey_, fileSize, generationId_)) {
    failAndCleanup();
    return false;
  }

  const auto localPath = epub_->getSpineItem(spineIndex_).href;
  const size_t lastSlash = localPath.find_last_of('/');
  const std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  const std::string imageBasePath = epub_->getCachePath() + "/img_" + std::to_string(spineIndex_) + "_";

  const CssParser* cssParser = nullptr;
  if (options_.embeddedStyle) {
    const uint32_t freeHeapBeforeCss = esp_get_free_heap_size();
    if (EpubIndexingPolicy::hasHeapForEmbeddedStyle(freeHeapBeforeCss)) {
      cssParser_ = makeUniqueNoThrow<CssParser>(epub_->getCachePath());
      if (!cssParser_) {
        LOG_ERR("ISB", "OOM: CssParser");
        failAndCleanup();
        return false;
      }
      cssParser = cssParser_.get();
      if (!cssParser_->loadFromCache()) {
        LOG_ERR("ISB", "Failed to load CSS from cache");
      }
      if (!EpubIndexingPolicy::hasHeapForEmbeddedStyle(esp_get_free_heap_size())) {
        cssParser_.reset();
        cssParser = nullptr;
      }
    }
  }

  parser_ = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub_, tmpHtmlPath_, renderer_, options_.fontId, options_.lineCompression, options_.extraParagraphSpacing,
      options_.paragraphAlignment, options_.viewportWidth, options_.viewportHeight, options_.hyphenationEnabled,
      options_.focusReadingEnabled,
      [this](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        if (!page) {
          appendFailed_ = true;
          cancelRequested_ = true;
          return;
        }
        const uint32_t pageNumber = knownPageCount_;
        if (!cache_.appendPage(pageNumber, *page, paragraphIndex, listItemIndex)) {
          appendFailed_ = true;
          cancelRequested_ = true;
          return;
        }
        knownPageCount_ = pageNumber + 1;
      },
      options_.embeddedStyle, contentBase, imageBasePath, options_.imageRendering, options_.popupFn, cssParser);
  if (!parser_) {
    LOG_ERR("ISB", "OOM: ChapterHtmlSlimParser");
    failAndCleanup();
    return false;
  }

  Hyphenator::setPreferredLanguage(epub_->getLanguage());
  if (!parser_->begin(&cancelRequested_)) {
    failAndCleanup();
    return false;
  }

  state_ = IncrementalBuildState::Parsing;
  LOG_DBG("ISB", "incremental-section start spine=%d bytes=%lu pages=%lu heap=%lu", spineIndex_,
          static_cast<unsigned long>(sourceFileSize_), static_cast<unsigned long>(knownPageCount_),
          static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

IncrementalBuildState IncrementalSectionBuilder::pump(const IncrementalBuildBudget& budget) {
  if (state_ == IncrementalBuildState::Idle && !start()) {
    return state_;
  }
  if (state_ != IncrementalBuildState::Parsing || !parser_) {
    return state_;
  }

  const uint32_t freeHeap = esp_get_free_heap_size();
  if (freeHeap < budget.minFreeHeap) {
    const uint32_t now = millis();
    if (now - lastDeferredLogMs_ >= LOW_HEAP_LOG_INTERVAL_MS) {
      LOG_DBG("ISB", "Deferring spine=%d heap=%lu need=%lu", spineIndex_, static_cast<unsigned long>(freeHeap),
              static_cast<unsigned long>(budget.minFreeHeap));
      lastDeferredLogMs_ = now;
    }
    return IncrementalBuildState::DeferredLowMemory;
  }

  ParsePumpBudget parseBudget;
  parseBudget.maxInputChunks = budget.maxInputChunks;
  parseBudget.maxCompletedPages = budget.maxCompletedPages;
  parseBudget.maxMillis = budget.maxMillis;
  const auto status = parser_->pump(parseBudget);
  if (appendFailed_) {
    failAndCleanup();
    return state_;
  }

  if (status == ParsePumpStatus::Complete) {
    if (!parser_->finish()) {
      failAndCleanup();
      return state_;
    }
    uint32_t anchorCount = 0;
    for (const auto& [anchor, page] : parser_->getAnchors()) {
      if (!cache_.appendAnchor(anchor, page)) {
        failAndCleanup();
        return state_;
      }
      anchorCount++;
    }
    if (!cache_.markComplete(knownPageCount_, anchorCount)) {
      failAndCleanup();
      return state_;
    }
    parser_.reset();
    cssParser_.reset();
    cleanupTempHtml();
    state_ = IncrementalBuildState::Complete;
    LOG_DBG("ISB", "incremental-section complete spine=%d pages=%lu", spineIndex_,
            static_cast<unsigned long>(knownPageCount_));
    return state_;
  }

  if (status == ParsePumpStatus::Cancelled || status == ParsePumpStatus::Error) {
    LOG_ERR("ISB", "incremental-section failed spine=%d status=%s", spineIndex_, parsePumpStatusName(status));
    failAndCleanup();
    return state_;
  }

  return IncrementalBuildState::Parsing;
}

void IncrementalSectionBuilder::cancel() {
  if (state_ == IncrementalBuildState::Complete || state_ == IncrementalBuildState::Cancelled ||
      state_ == IncrementalBuildState::Failed) {
    return;
  }
  cancelRequested_ = true;
  if (parser_) {
    parser_->close();
    parser_.reset();
  }
  cssParser_.reset();
  if (state_ == IncrementalBuildState::Parsing || state_ == IncrementalBuildState::Extracting) {
    cache_.removeAllFiles();
  }
  cleanupTempHtml();
  knownPageCount_ = 0;
  state_ = IncrementalBuildState::Cancelled;
}

bool IncrementalSectionBuilder::hasPage(const uint32_t pageNumber) const { return pageNumber < knownPageCount_; }

std::unique_ptr<Page> IncrementalSectionBuilder::loadPage(const uint32_t pageNumber) const {
  return cache_.loadPage(pageNumber);
}

uint32_t IncrementalSectionBuilder::knownPageCount() const { return knownPageCount_; }

bool IncrementalSectionBuilder::isComplete() const { return state_ == IncrementalBuildState::Complete; }

void IncrementalSectionBuilder::failAndCleanup() {
  if (parser_) {
    parser_->close();
    parser_.reset();
  }
  cssParser_.reset();
  cache_.removeAllFiles();
  cleanupTempHtml();
  knownPageCount_ = 0;
  state_ = IncrementalBuildState::Failed;
}
