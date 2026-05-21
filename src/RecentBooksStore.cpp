#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>

namespace {
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr int MAX_RECENT_BOOKS = 10;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& series, const std::string& coverBmpPath) {
  int8_t embeddedStyleOverride = -1;
  int8_t imageRenderingOverride = -1;
  int8_t fontFamilyOverride = -1;
  std::string sdFontFamilyOverride;
  int8_t fontSizeOverride = -1;
  int8_t bionicReadingOverride = -1;
  int8_t paragraphAlignmentOverride = -1;

  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    embeddedStyleOverride = it->embeddedStyleOverride;
    imageRenderingOverride = it->imageRenderingOverride;
    fontFamilyOverride = it->fontFamilyOverride;
    sdFontFamilyOverride = it->sdFontFamilyOverride;
    fontSizeOverride = it->fontSizeOverride;
    bionicReadingOverride = it->bionicReadingOverride;
    paragraphAlignmentOverride = it->paragraphAlignmentOverride;
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, series, coverBmpPath, embeddedStyleOverride,
                                           imageRenderingOverride, fontFamilyOverride, sdFontFamilyOverride,
                                           fontSizeOverride, bionicReadingOverride, paragraphAlignmentOverride});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::removeBook(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
    saveToFile();
  }
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& series, const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.series = series;
    book.coverBmpPath = coverBmpPath;
    saveToFile();
  }
}

RecentBook RecentBooksStore::getBookByPath(const std::string& path) const {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    return *it;
  }
  return RecentBook{};
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, it->fontFamilyOverride,
                            it->sdFontFamilyOverride, it->fontSizeOverride, it->bionicReadingOverride,
                            it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const int8_t fontSizeOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  const std::string sdOverride = (fontFamilyOverride >= 0) ? std::string() : it->sdFontFamilyOverride;
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride, sdOverride,
                            fontSizeOverride, it->bionicReadingOverride, it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride,
                            sdFontFamilyOverride, fontSizeOverride, it->bionicReadingOverride,
                            it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const bool bionicReadingOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, it->fontFamilyOverride,
                            it->sdFontFamilyOverride, it->fontSizeOverride, bionicReadingOverride,
                            it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const int8_t fontSizeOverride, const bool bionicReadingOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  const std::string sdOverride = (fontFamilyOverride >= 0) ? std::string() : it->sdFontFamilyOverride;
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride, sdOverride,
                            fontSizeOverride, bionicReadingOverride, it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride,
                                          const bool bionicReadingOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride,
                            sdFontFamilyOverride, fontSizeOverride, bionicReadingOverride,
                            it->paragraphAlignmentOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride,
                                          const bool bionicReadingOverride, const int8_t paragraphAlignmentOverride) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }

  it->embeddedStyleOverride = embeddedStyleOverride;
  it->imageRenderingOverride = imageRenderingOverride;
  it->fontFamilyOverride = fontFamilyOverride;
  it->sdFontFamilyOverride = sdFontFamilyOverride;
  it->fontSizeOverride = fontSizeOverride;
  it->bionicReadingOverride = bionicReadingOverride;
  it->paragraphAlignmentOverride = paragraphAlignmentOverride;
  return saveToFile();
}

bool RecentBooksStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRecentBooks(*this, RECENT_BOOKS_FILE_JSON);
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    std::string series = epub.getSeries();
    if (!series.empty() && !epub.getSeriesIndex().empty()) series += " #" + epub.getSeriesIndex();
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), series, epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), "", xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", "", ""};
  }
  return RecentBook{path, "", "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }
  return false;
}
