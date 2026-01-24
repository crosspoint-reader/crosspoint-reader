#include "BookCoverCache.h"
#include <Epub.h>
#include <Xtc.h>
#include "util/StringUtils.h"
#include <sys/stat.h>

BookCoverCache::BookCoverCache(const std::string& cache_dir, GfxRenderer* renderer)
    : cache_dir(cache_dir), renderer(renderer) {
  // Ensure the cache directory exists
  if (!SdMan.exists(cache_dir.c_str())) {
    SdMan.mkdir(cache_dir.c_str());
  }
}

BookCoverCache::~BookCoverCache() {
}



bool BookCoverCache::render(const std::string& book_path, int x, int y, int width,
                            int height) {
  auto bitmap = getCover(book_path);
  if (bitmap) {
    renderer->drawBitmap(*bitmap, x, y, width, height);
    return true;
  }
  return false;
}

std::shared_ptr<Bitmap> BookCoverCache::getCover(const std::string& book_path) {
  std::string cache_path = getCachePath(book_path);

  if (isCacheValid(book_path)) {
    FsFile file;
    if (SdMan.openFileForRead("CACHE", cache_path, file)) {
      auto bitmap = std::make_shared<Bitmap>(file);
      if (bitmap->parseHeaders() == BmpReaderError::Ok) {
        return bitmap;
      }
    }
  }

  return generateThumbnail(book_path);
}

void BookCoverCache::clearCache() {
  // TODO: Implement this
}

std::string BookCoverCache::getCachePath(const std::string& book_path) const {
  // Simple cache path: replace '/' with '_' and append .bmp
  std::string safe_path = book_path;
  std::replace(safe_path.begin(), safe_path.end(), '/', '_');
  return cache_dir + "/" + safe_path + ".bmp";
}

bool BookCoverCache::isCacheValid(const std::string& book_path) const {
    std::string cache_path = getCachePath(book_path);
    return SdMan.exists(cache_path.c_str());
}


void BookCoverCache::setTargetSize(int width, int height) {
  target_width = width;
  target_height = height;
}

std::shared_ptr<Bitmap> BookCoverCache::generateThumbnail(const std::string& book_path) {
    std::string coverBmpPath;
    bool hasCoverImage = false;

    if (StringUtils::checkFileExtension(book_path, ".epub")) {
      Epub epub(book_path, "/.crosspoint/epub_cache");
      if (epub.load(false) && epub.generateThumbBmp()) {
        coverBmpPath = epub.getThumbBmpPath();
        hasCoverImage = true;
      }
    } else if (StringUtils::checkFileExtension(book_path, ".xtch") ||
               StringUtils::checkFileExtension(book_path, ".xtc")) {
      Xtc xtc(book_path, "/.crosspoint/xtc_cache");
      if (xtc.load() && xtc.generateThumbBmp()) {
        coverBmpPath = xtc.getThumbBmpPath();
        hasCoverImage = true;
      }
    }

    if (hasCoverImage && !coverBmpPath.empty()) {
        std::string cache_path = getCachePath(book_path);
        // This is not very efficient, we are copying the file.
        // We should ideally generate the thumbnail directly to the cache path.
        // For now, this will do.
        FsFile source_file;
        FsFile dest_file;
        if (SdMan.openFileForRead("THUMB", coverBmpPath, source_file) &&
            SdMan.openFileForWrite("THUMB", cache_path, dest_file)) {
          uint8_t buffer[512];
          while (source_file.available()) {
            int bytes_read = source_file.read(buffer, sizeof(buffer));
            if (bytes_read > 0) {
              dest_file.write(buffer, bytes_read);
            }
          }
          source_file.close();
          dest_file.close();

          FsFile file;
          if (SdMan.openFileForRead("CACHE", cache_path, file)) {
            auto bitmap = std::make_shared<Bitmap>(file);
            if (bitmap->parseHeaders() == BmpReaderError::Ok) {
              return bitmap;
            }
          }
        }
    }

    return nullptr;
}
