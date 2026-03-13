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

  static std::string formatFileSize(size_t bytes);
  void renderLoading();
  void loadData();

 public:
  explicit BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
      : Activity("BookInfo", renderer, mappedInput), filePath(std::move(filePath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
