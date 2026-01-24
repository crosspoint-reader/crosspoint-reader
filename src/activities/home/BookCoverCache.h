#pragma once

#include <string>
#include <memory>
#include <vector>
#include <Bitmap.h>
#include <SDCardManager.h>
#include <GfxRenderer.h>

class BookCoverCache {
 public:
  BookCoverCache(const std::string& cache_dir, GfxRenderer* renderer);
  ~BookCoverCache();

  bool render(const std::string& book_path, int x, int y, int width, int height);
  void clearCache();
  void setTargetSize(int width, int height);

 private:
  std::string getCachePath(const std::string& book_path) const;
  bool generateThumbnail(const std::string& book_path);
  
  std::string cache_dir;
  GfxRenderer* renderer;
  int target_width;
  int target_height;
};
