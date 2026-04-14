#pragma once

#include <string>
#include <vector>

#include "../Activity.h"

class BookInfoActivity final : public Activity {
  const std::string filePath;

  // Metadata populated in onEnter
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string description;
  std::string coverBmpPath;
  std::string loadError;
  bool loadSucceeded = false;
  size_t fileSizeBytes = 0;

  // Description paging (populated lazily on first render)
  std::vector<std::string> descLines;
  int descWrappedWidth = 0;
  int descPage = 0;
  int descLinesPerPage = 0;
  int descTotalPages = 0;

  // Partial-render caches: set on the first full render, reused when only the
  // description page changes.
  bool fullRenderDone = false;
  int descBandX = 0;
  int descBandY = 0;
  int descBandWidth = 0;
  int descBandHeight = 0;
  int hintsBandY = 0;
  int hintsBandHeight = 0;
  int partialRenderCount = 0;
  static constexpr int MAX_PARTIAL_RENDERS = 10;

  static std::string formatFileSize(size_t bytes);
  void renderLoading();
  void loadData();
  void renderDescriptionAndHints();

 public:
  explicit BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
      : Activity("BookInfo", renderer, mappedInput), filePath(std::move(filePath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
