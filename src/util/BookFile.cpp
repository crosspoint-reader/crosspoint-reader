#include "BookFile.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

namespace {

std::string fileNameOf(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
}

}  // namespace

BookFormat bookFormat(const std::string_view path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return BookFormat::Epub;
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return BookFormat::Xtc;
  }
  // Markdown is read by the TXT reader, so it counts as the same format here.
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return BookFormat::Txt;
  }
  return BookFormat::Unknown;
}

bool isBook(const std::string_view path) { return bookFormat(path) != BookFormat::Unknown; }

BookInfo readBookInfo(const std::string& path) {
  LOG_DBG("BookFile", "Reading book info: %s", path.c_str());

  switch (bookFormat(path)) {
    case BookFormat::Epub: {
      Epub epub(path, BOOK_CACHE_ROOT);
      epub.load(false, true);
      return {epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
    }
    case BookFormat::Xtc: {
      Xtc xtc(path, BOOK_CACHE_ROOT);
      if (!xtc.load()) {
        return {};
      }
      return {xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
    case BookFormat::Txt:
      // Reading the file would cost SD I/O on boot for a title the name already carries.
      return {fileNameOf(path), "", ""};
    case BookFormat::Unknown:
      return {};
  }
  return {};
}

bool hasThumbnail(const BookFormat format) {
  switch (format) {
    case BookFormat::Epub:
    case BookFormat::Xtc:
      return true;
    case BookFormat::Txt:
    case BookFormat::Unknown:
      return false;
  }
  return false;
}

bool generateBookThumb(const std::string& path, const int height) {
  switch (bookFormat(path)) {
    case BookFormat::Epub: {
      Epub epub(path, BOOK_CACHE_ROOT);
      epub.load(false, true);
      return epub.generateThumbBmp(height);
    }
    case BookFormat::Xtc: {
      Xtc xtc(path, BOOK_CACHE_ROOT);
      return xtc.load() && xtc.generateThumbBmp(height);
    }
    case BookFormat::Txt:
    case BookFormat::Unknown:
      return false;
  }
  return false;
}

std::string generateBookCoverBmp(const std::string& path, const bool cropped) {
  switch (bookFormat(path)) {
    case BookFormat::Epub: {
      Epub epub(path, BOOK_CACHE_ROOT);
      if (!epub.load(true, true)) {
        LOG_ERR("BookFile", "Failed to load EPUB: %s", path.c_str());
        return {};
      }
      if (!epub.generateCoverBmp(cropped)) {
        LOG_ERR("BookFile", "Failed to generate EPUB cover bmp: %s", path.c_str());
        return {};
      }
      return epub.getCoverBmpPath(cropped);
    }
    case BookFormat::Xtc: {
      Xtc xtc(path, BOOK_CACHE_ROOT);
      if (!xtc.load()) {
        LOG_ERR("BookFile", "Failed to load XTC: %s", path.c_str());
        return {};
      }
      if (!xtc.generateCoverBmp()) {
        LOG_ERR("BookFile", "Failed to generate XTC cover bmp: %s", path.c_str());
        return {};
      }
      return xtc.getCoverBmpPath();
    }
    case BookFormat::Txt: {
      // The cover is a sibling image file, not something stored in the book.
      Txt txt(path, BOOK_CACHE_ROOT);
      if (!txt.load()) {
        LOG_ERR("BookFile", "Failed to load TXT: %s", path.c_str());
        return {};
      }
      if (!txt.generateCoverBmp()) {
        LOG_DBG("BookFile", "No cover image found for TXT: %s", path.c_str());
        return {};
      }
      return txt.getCoverBmpPath();
    }
    case BookFormat::Unknown:
      return {};
  }
  return {};
}
