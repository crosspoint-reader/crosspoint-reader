#include "RecentBooksStore.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_JSON[] = "/.crosspoint/recent.json";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr int MAX_RECENT_BOOKS = 10;
constexpr int16_t UNKNOWN_PROGRESS_PERCENT = -1;
constexpr uint32_t INITIAL_READING_SECONDS = 0;
constexpr int BASE_CONTENT_WIDTH_OFFSET = 5;
constexpr int SUBTITLE_RIGHT_GAP = 8;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  int16_t progressPercent = UNKNOWN_PROGRESS_PERCENT;
  uint32_t readingSeconds = INITIAL_READING_SECONDS;

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    progressPercent = it->progressPercent;
    readingSeconds = it->readingSeconds;
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath, progressPercent, readingSeconds});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  refreshComputedFields(recentBooks.front());
  saveToFile();
}

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    RecentBook& book = *it;
    book.title = title;
    book.author = author;
    book.coverBmpPath = coverBmpPath;
    refreshComputedFields(book);
    saveToFile();
  }
}

void RecentBooksStore::updateBookProgress(const std::string& path, const int16_t progressPercent) {
  const int16_t clamped = std::max<int16_t>(0, std::min<int16_t>(100, progressPercent));
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end() || it->progressPercent == clamped) {
    return;
  }

  it->progressPercent = clamped;
  refreshComputedFields(*it);
  saveToFile();
}

void RecentBooksStore::addBookReadingTime(const std::string& path, const uint32_t elapsedSeconds) {
  if (elapsedSeconds == 0) {
    return;
  }

  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return;
  }

  const uint32_t previous = it->readingSeconds;
  it->readingSeconds += elapsedSeconds;
  if (it->readingSeconds < previous) {
    it->readingSeconds = UINT32_MAX;
  }
  refreshComputedFields(*it);
  saveToFile();
}

std::vector<RecentBookListRowData> RecentBooksStore::buildListRowData(const std::vector<RecentBook>& books,
                                                                      const GfxRenderer& renderer,
                                                                      const ThemeMetrics& metrics, const int pageWidth,
                                                                      const int contentHeight) const {
  std::vector<RecentBookListRowData> rows;
  rows.reserve(books.size());
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int pageItems = std::max(1, contentHeight / rowHeight);
  const int totalPages = (static_cast<int>(books.size()) + pageItems - 1) / pageItems;
  const int contentWidth = pageWidth - (totalPages > 1 ? (metrics.scrollBarWidth + metrics.scrollBarRightOffset)
                                                       : BASE_CONTENT_WIDTH_OFFSET);
  const int subtitleFont = UI_10_FONT_ID;
  const int subtitleLeftX = metrics.contentSidePadding;

  for (const auto& book : books) {
    RecentBookListRowData row;
    row.metricsRightText = book.metricsSubtitle;
    const int rightWidth = renderer.getTextWidth(subtitleFont, row.metricsRightText.c_str());
    row.metricsRightX = contentWidth - metrics.contentSidePadding - rightWidth;
    const int maxAuthorWidth = std::max(0, row.metricsRightX - subtitleLeftX - SUBTITLE_RIGHT_GAP);
    row.authorSubtitle = renderer.truncatedText(subtitleFont, book.author.c_str(), maxAuthorWidth);
    rows.push_back(std::move(row));
  }

  return rows;
}

void RecentBooksStore::drawMetricsOverlay(const GfxRenderer& renderer,
                                          const std::vector<RecentBookListRowData>& rowData, const size_t selectorIndex,
                                          const int contentTop, const int contentHeight,
                                          const ThemeMetrics& metrics) const {
  if (rowData.empty()) {
    return;
  }
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int pageItems = std::max(1, contentHeight / rowHeight);
  const int pageStartIndex = static_cast<int>(selectorIndex) / pageItems * pageItems;
  const int subtitleFont = UI_10_FONT_ID;

  for (int i = pageStartIndex; i < static_cast<int>(rowData.size()) && i < pageStartIndex + pageItems; i++) {
    renderer.drawText(subtitleFont, rowData[i].metricsRightX, contentTop + (i % pageItems) * rowHeight + 30,
                      rowData[i].metricsRightText.c_str(), true);
  }
}

int RecentBooksStore::getBookProgressPercent(const RecentBook& book) const {
  return clampProgressPercent(book.progressPercent);
}

std::string RecentBooksStore::getBookProgressValue(const RecentBook& book) const { return book.progressValue; }

std::string RecentBooksStore::getBookReadingTime(const RecentBook& book) const {
  return formatDuration(book.readingSeconds);
}

std::string RecentBooksStore::getBookMetricsSubtitle(const RecentBook& book) const { return book.metricsSubtitle; }

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
  if (StringUtils::checkFileExtension(lastBookFileName, ".epub")) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path,
                      epub.getTitle(),
                      epub.getAuthor(),
                      epub.getThumbBmpPath(),
                      UNKNOWN_PROGRESS_PERCENT,
                      INITIAL_READING_SECONDS};
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtch") ||
             StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path,
                        xtc.getTitle(),
                        xtc.getAuthor(),
                        xtc.getThumbBmpPath(),
                        UNKNOWN_PROGRESS_PERCENT,
                        INITIAL_READING_SECONDS};
    }
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".txt") ||
             StringUtils::checkFileExtension(lastBookFileName, ".md")) {
    return RecentBook{path, lastBookFileName, "", "", UNKNOWN_PROGRESS_PERCENT, INITIAL_READING_SECONDS};
  }
  return RecentBook{path, "", "", "", UNKNOWN_PROGRESS_PERCENT, INITIAL_READING_SECONDS};
}

bool RecentBooksStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(RECENT_BOOKS_FILE_JSON)) {
    String json = Storage.readFile(RECENT_BOOKS_FILE_JSON);
    if (!json.isEmpty()) {
      if (JsonSettingsIO::loadRecentBooks(*this, json.c_str())) {
        refreshAllComputedFields();
        return true;
      }
    }
  }

  // Fall back to binary migration
  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, "", UNKNOWN_PROGRESS_PERCENT, INITIAL_READING_SECONDS});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == 3) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      recentBooks.push_back({path, title, author, coverBmpPath, UNKNOWN_PROGRESS_PERCENT, INITIAL_READING_SECONDS});
    }

    if (omitted > 0) {
      inputFile.close();
      saveToFile();
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return false;
  }

  inputFile.close();
  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  refreshAllComputedFields();
  return true;
}

void RecentBooksStore::refreshComputedFields(RecentBook& book) {
  if (book.progressPercent < 0) {
    book.progressValue = "--%";
  } else {
    book.progressValue = std::to_string(clampProgressPercent(book.progressPercent)) + "%";
  }
  const std::string readText = formatDuration(book.readingSeconds);
  const std::string remainingText = hasRemainingEstimate(book) ? formatDuration(getRemainingSeconds(book)) : "--";
  book.metricsSubtitle = readText + " · " + remainingText;
}

void RecentBooksStore::refreshAllComputedFields() {
  for (auto& book : recentBooks) {
    refreshComputedFields(book);
  }
}

std::string RecentBooksStore::formatDuration(const uint32_t totalSeconds) {
  const uint32_t hours = totalSeconds / 3600U;
  const uint32_t minutes = (totalSeconds % 3600U) / 60U;
  return std::to_string(hours) + "h " + (minutes < 10 ? "0" : "") + std::to_string(minutes) + "m";
}

int RecentBooksStore::clampProgressPercent(const int progressPercent) {
  return std::min(100, std::max(0, progressPercent));
}

bool RecentBooksStore::hasRemainingEstimate(const RecentBook& book) {
  return book.progressPercent >= 100 || (book.progressPercent > 0 && book.readingSeconds > 0);
}

uint32_t RecentBooksStore::getRemainingSeconds(const RecentBook& book) {
  if (book.progressPercent >= 100) {
    return 0;
  }

  if (book.progressPercent <= 0 || book.readingSeconds == 0) {
    return 0;
  }

  const float ratio = static_cast<float>(book.progressPercent) / 100.0f;
  const float estimateTotal = static_cast<float>(book.readingSeconds) / ratio;
  const uint32_t estimateTotalSeconds = static_cast<uint32_t>(estimateTotal + 0.5f);
  return (estimateTotalSeconds > book.readingSeconds) ? (estimateTotalSeconds - book.readingSeconds) : 0;
}
