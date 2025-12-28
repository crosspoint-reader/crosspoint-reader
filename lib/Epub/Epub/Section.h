#pragma once
#include <functional>
#include <memory>

#include "Epub.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string cachePath;

  void writeCacheMetadata(int fontId, float lineCompression, int marginTop, int marginRight, int marginBottom,
                          int marginLeft, bool extraParagraphSpacing, int screenWidth, int screenHeight) const;
  void onPageComplete(std::unique_ptr<Page> page);

 public:
  int pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        cachePath(epub->getCachePath() + "/" + std::to_string(spineIndex)) {}
  ~Section() = default;
  bool loadCacheMetadata(int fontId, float lineCompression, int marginTop, int marginRight, int marginBottom,
                         int marginLeft, bool extraParagraphSpacing, int screenWidth, int screenHeight);
  void setupCacheDir() const;
  bool clearCache() const;
  bool persistPageDataToSD(int fontId, float lineCompression, int marginTop, int marginRight, int marginBottom,
                           int marginLeft, bool extraParagraphSpacing, int screenWidth, int screenHeight,
                           const std::function<void()>& progressSetupFn = nullptr,
                           const std::function<void(int)>& progressFn = nullptr);
  std::unique_ptr<Page> loadPageFromSD() const;
};
