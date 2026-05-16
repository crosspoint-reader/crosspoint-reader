#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "IncrementalBuildBudget.h"
#include "IncrementalSectionCache.h"
#include "parsers/ChapterHtmlSlimParser.h"

class Epub;
class GfxRenderer;
class Page;

enum class IncrementalBuildState {
  Idle,
  Extracting,
  Parsing,
  Complete,
  Cancelled,
  Failed,
  DeferredLowMemory,
};

struct IncrementalBuildOptions {
  int fontId = 0;
  float lineCompression = 1.0F;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  std::function<void()> popupFn = nullptr;
};

class IncrementalSectionBuilder {
 public:
  IncrementalSectionBuilder(std::shared_ptr<Epub> epub, int spineIndex, GfxRenderer& renderer,
                            IncrementalSection::LayoutCacheKey layoutKey, IncrementalBuildOptions options);
  ~IncrementalSectionBuilder();

  bool start();
  IncrementalBuildState pump(const IncrementalBuildBudget& budget);
  void cancel();
  bool hasPage(uint32_t pageNumber) const;
  std::unique_ptr<Page> loadPage(uint32_t pageNumber) const;
  uint32_t knownPageCount() const;
  bool isComplete() const;
  IncrementalBuildState state() const { return state_; }
  int spineIndex() const { return spineIndex_; }

 private:
  std::shared_ptr<Epub> epub_;
  int spineIndex_;
  GfxRenderer& renderer_;
  IncrementalSection::LayoutCacheKey layoutKey_;
  IncrementalBuildOptions options_;
  IncrementalSection::Cache cache_;
  std::unique_ptr<ChapterHtmlSlimParser> parser_;
  std::unique_ptr<CssParser> cssParser_;
  std::string tmpHtmlPath_;
  bool cancelRequested_ = false;
  bool appendFailed_ = false;
  uint32_t generationId_ = 0;
  IncrementalBuildState state_ = IncrementalBuildState::Idle;
  uint32_t sourceFileSize_ = 0;
  uint32_t knownPageCount_ = 0;
  uint32_t lastDeferredLogMs_ = 0;

  bool extractTempHtml(uint32_t& fileSize);
  void cleanupTempHtml() const;
  void failAndCleanup();
};
