#include "RecentBooksStore.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>

#include "util/StringUtils.h"

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE[] = "/.crosspoint/recent.bin";
constexpr int MAX_RECENT_BOOKS = 10;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

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
    saveToFile();
  }
}

bool RecentBooksStore::saveToFile() const {
  // Make sure the directory exists
  Storage.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!Storage.openFileForWrite("RBS", RECENT_BOOKS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, RECENT_BOOKS_FILE_VERSION);
  const uint8_t count = static_cast<uint8_t>(recentBooks.size());
  serialization::writePod(outputFile, count);

  for (const auto& book : recentBooks) {
    serialization::writeString(outputFile, book.path);
    serialization::writeString(outputFile, book.title);
    serialization::writeString(outputFile, book.author);
    serialization::writeString(outputFile, book.coverBmpPath);
  }

  outputFile.close();
  LOG_DBG("RBS", "Recent books saved to file (%d entries)", count);
  return true;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover
  if (StringUtils::checkFileExtension(lastBookFileName, ".epub")) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".xtch") ||
             StringUtils::checkFileExtension(lastBookFileName, ".xtc")) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (StringUtils::checkFileExtension(lastBookFileName, ".txt") ||
             StringUtils::checkFileExtension(lastBookFileName, ".md")) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != RECENT_BOOKS_FILE_VERSION) {
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
          recentBooks.push_back({path, title, author, ""});
        } else {
          recentBooks.push_back(book);
        }
      }
    } else {
      LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
      inputFile.close();
      return false;
    }
  } else {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);
      recentBooks.push_back({path, title, author, coverBmpPath});
    }
  }

  inputFile.close();
  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", recentBooks.size());
  return true;
}
