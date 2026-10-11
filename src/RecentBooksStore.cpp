#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <SidecarFiles.h>
#include <Xtc.h>

#include <algorithm>

#include "activities/home/BookDetails.h"

namespace {
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr int MAX_RECENT_BOOKS = 10;

// "Series #3", or just "Series" without an index.
std::string seriesLabel(const std::string& series, const std::string& index) {
  return !series.empty() && !index.empty() ? series + " #" + index : series;
}

std::string seriesLabel(const Epub& epub) { return seriesLabel(epub.getSeries(), epub.getSeriesIndex()); }
}  // namespace

RecentBooksStore RecentBooksStore::instance;

RecentBooksStore::Hold::Hold() {
  ++instance.holds;
  instance.ensureLoaded();
}

RecentBooksStore::Hold::~Hold() {
  if (instance.holds > 0) --instance.holds;
  if (instance.holds == 0 && instance.loaded) {
    std::vector<RecentBook>().swap(instance.recentBooks);  // clear() would keep the capacity
    instance.loaded = false;
  }
}

void RecentBooksStore::ensureLoaded() const {
  if (loaded) return;
  // loadFromFile() is the one place the list is filled; const only to the caller's eye.
  const_cast<RecentBooksStore*>(this)->loadFromFile();
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& series, const std::string& coverBmpPath) {
  const Hold hold;
  RecentBook newBook{path, title, author, series, coverBmpPath};
  // The EPUB and text readers pass metadata they have just loaded, sidecar applied; stamp the sidecar
  // it came from so refreshSidecarMetadata() does not redo it. The XTC reader passes the book's own
  // header, so an XTC is left unstamped and its sidecar, if any, applied by the refresh.
  if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    newBook.metadataStamp = SidecarFiles::metadataStamp(path);
  }

  pruneMissing();

  // Remove existing entry if present, preserving its per-book overrides.
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    newBook.embeddedStyleOverride = it->embeddedStyleOverride;
    newBook.imageRenderingOverride = it->imageRenderingOverride;
    newBook.fontFamilyOverride = it->fontFamilyOverride;
    newBook.sdFontFamilyOverride = it->sdFontFamilyOverride;
    newBook.fontSizeOverride = it->fontSizeOverride;
    newBook.bionicReadingOverride = it->bionicReadingOverride;
    newBook.paragraphAlignmentOverride = it->paragraphAlignmentOverride;
    newBook.textAntiAliasingOverride = it->textAntiAliasingOverride;
    newBook.hyphenationOverride = it->hyphenationOverride;
    newBook.fontSizeNormalizationOverride = it->fontSizeNormalizationOverride;
    newBook.guideDotsOverride = it->guideDotsOverride;
    newBook.inlineFootnotePreviewsOverride = it->inlineFootnotePreviewsOverride;
    recentBooks.erase(it);
  }

  recentBooks.insert(recentBooks.begin(), std::move(newBook));

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

void RecentBooksStore::removeBook(const std::string& path) {
  const Hold hold;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
    saveToFile();
  }
}

void RecentBooksStore::relocate(const std::string& oldPath, const std::string& newPath, const std::string& oldCacheDir,
                                const std::string& newCacheDir) {
  const Hold hold;
  const auto at = [](const std::string& path) { return [&path](const RecentBook& book) { return book.path == path; }; };
  if (std::none_of(recentBooks.begin(), recentBooks.end(), at(oldPath))) return;
  // A row left for a book deleted from the new path would now name this one twice.
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), at(newPath)), recentBooks.end());

  RecentBook& book = *std::find_if(recentBooks.begin(), recentBooks.end(), at(oldPath));
  book.path = newPath;
  const std::string oldPrefix = oldCacheDir + "/";
  if (!oldCacheDir.empty() && book.coverBmpPath.compare(0, oldPrefix.size(), oldPrefix) == 0) {
    book.coverBmpPath = newCacheDir + book.coverBmpPath.substr(oldCacheDir.size());
  }
  // TXT and Markdown books are listed under their file name.
  const auto nameOf = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
  if (book.title == nameOf(oldPath)) book.title = nameOf(newPath);
  saveToFile();
}

bool RecentBooksStore::refreshSidecarMetadata(const size_t maxBooks) {
  const Hold hold;
  bool changed = false;
  size_t seen = 0;
  for (RecentBook& book : recentBooks) {
    // Counted as the home screen counts them, which skips missing books.
    if (seen >= maxBooks) break;
    if (isMissing(book)) continue;
    seen++;
    const uint32_t stamp = SidecarFiles::metadataStamp(book.path);
    if (stamp == book.metadataStamp) continue;

    if (FsHelpers::hasEpubExtension(book.path)) {
      // The same load the reader's metadata comes from, so a removed sidecar
      // restores the embedded values rather than leaving the old override.
      Epub epub(book.path, "/.crosspoint");
      if (!epub.loadForMetadata()) {
        LOG_DBG("RBS", "Sidecar metadata changed but book did not load, keeping entry: %s", book.path.c_str());
        continue;  // stamp left stale: retried next time
      }
      book.title = epub.getTitle();
      book.author = epub.getAuthor();
      book.series = seriesLabel(epub);
    } else {
      // XTC, TXT and Markdown: the lookup the Library's Details view uses, the sidecar over the book's
      // own header (XTC) or file name (TXT, Markdown). Edits made in the web interface's Details
      // dialog reached Home for EPUBs only.
      BookDetails details;
      if (!BookDetailsLookup::cached(book.path, 0, details) && !BookDetailsLookup::parse(book.path, 0, details)) {
        continue;  // stamp left stale: retried next time
      }
      // No title (a TXT or Markdown book whose sidecar was removed) means the file name, as the
      // reader lists it (LineReaderActivity); keeping the old one would show the removed title.
      book.title = details.title.empty() ? book.path.substr(book.path.rfind('/') + 1) : details.title;
      book.author = details.author;
      book.series = seriesLabel(details.series, details.seriesIndex);
    }
    LOG_DBG("RBS", "Sidecar metadata changed, refreshing entry: %s", book.path.c_str());
    book.metadataStamp = stamp;
    changed = true;
  }
  if (changed) saveToFile();
  return changed;
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::pruneMissing() {
  const Hold hold;
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& series, const std::string& coverBmpPath) {
  const Hold hold;
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
  const Hold hold;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    return *it;
  }
  return RecentBook{};
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride) {
  const Hold hold;
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
  const Hold hold;
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
  const Hold hold;
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
  const Hold hold;
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
  const Hold hold;
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
  const Hold hold;
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
  const Hold hold;
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  return setReaderOverrides(path, embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride,
                            sdFontFamilyOverride, fontSizeOverride, static_cast<int8_t>(bionicReadingOverride ? 1 : 0),
                            paragraphAlignmentOverride, it->textAntiAliasingOverride, it->hyphenationOverride,
                            it->fontSizeNormalizationOverride, it->guideDotsOverride,
                            it->inlineFootnotePreviewsOverride);
}

bool RecentBooksStore::setReaderOverrides(const std::string& path, const int8_t embeddedStyleOverride,
                                          const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                          const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride,
                                          const int8_t bionicReadingOverride, const int8_t paragraphAlignmentOverride,
                                          const int8_t textAntiAliasingOverride, const int8_t hyphenationOverride,
                                          const int8_t fontSizeNormalizationOverride, const int8_t guideDotsOverride,
                                          const int8_t inlineFootnotePreviewsOverride) {
  const Hold hold;
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
  it->textAntiAliasingOverride = textAntiAliasingOverride;
  it->hyphenationOverride = hyphenationOverride;
  it->fontSizeNormalizationOverride = fontSizeNormalizationOverride;
  it->guideDotsOverride = guideDotsOverride;
  it->inlineFootnotePreviewsOverride = inlineFootnotePreviewsOverride;
  return saveToFile();
}

bool RecentBooksStore::saveToFile() const {
  if (!loaded) {
    LOG_ERR("RBS", "Not saving recent books: the list is not loaded, and would overwrite the file empty");
    return false;
  }
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
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), seriesLabel(epub), epub.getThumbBmpPath()};
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
  // Loaded from here on, file or no file: no file (or a broken one) is an empty list, which a
  // later addBook() then saves as the new file.
  std::vector<RecentBook>().swap(recentBooks);
  loaded = true;
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadRecentBooks(*this, json.c_str());
    }
  }
  return false;
}
