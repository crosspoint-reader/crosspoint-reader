#include "RecentBooksStore.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 1;
constexpr char RECENT_BOOKS_FILE[] = "/.crosspoint/recent.bin";
constexpr int MAX_RECENT_BOOKS = 10;
}  // namespace

RecentBooksStore RecentBooksStore::instance;

void RecentBooksStore::addBook(const std::string& path) {
  // Remove existing entry if present
  auto it = std::find(recentBooks.begin(), recentBooks.end(), path);
  if (it != recentBooks.end()) {
    recentBooks.erase(it);
  }

  // Add to front
  recentBooks.insert(recentBooks.begin(), path);

  // Trim to max size
  if (recentBooks.size() > MAX_RECENT_BOOKS) {
    recentBooks.resize(MAX_RECENT_BOOKS);
  }

  saveToFile();
}

bool RecentBooksStore::saveToFile() const {
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("RBS", RECENT_BOOKS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, RECENT_BOOKS_FILE_VERSION);
  const uint8_t count = static_cast<uint8_t>(recentBooks.size());
  serialization::writePod(outputFile, count);

  for (const auto& book : recentBooks) {
    serialization::writeString(outputFile, book);
  }

  outputFile.close();
  Serial.printf("[%lu] [RBS] Recent books saved to file (%d entries)\n", millis(), count);
  return true;
}

bool RecentBooksStore::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("RBS", RECENT_BOOKS_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != RECENT_BOOKS_FILE_VERSION) {
    Serial.printf("[%lu] [RBS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t count;
  serialization::readPod(inputFile, count);

  recentBooks.clear();
  recentBooks.reserve(count);

  for (uint8_t i = 0; i < count; i++) {
    std::string path;
    serialization::readString(inputFile, path);
    recentBooks.push_back(path);
  }

  inputFile.close();
  Serial.printf("[%lu] [RBS] Recent books loaded from file (%d entries)\n", millis(), count);
  return true;
}
