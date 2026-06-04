#include "RssFeedStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

RssFeedStore RssFeedStore::instance;

namespace {
constexpr char RSS_FILE_JSON[] = "/.crosspoint/rss.json";
}

bool RssFeedStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveRss(*this, RSS_FILE_JSON);
}

bool RssFeedStore::loadFromFile() {
  if (!Storage.exists(RSS_FILE_JSON)) return false;

  String json = Storage.readFile(RSS_FILE_JSON);
  if (json.isEmpty()) return false;

  bool resave = false;
  const bool result = JsonSettingsIO::loadRss(*this, json.c_str(), &resave);
  if (result && resave) {
    LOG_DBG("RSS", "Resaving JSON with obfuscated passwords");
    saveToFile();
  }
  return result;
}

bool RssFeedStore::addFeed(const RssFeed& feed) {
  if (feeds.size() >= MAX_FEEDS) {
    LOG_DBG("RSS", "Cannot add more feeds, limit of %zu reached", MAX_FEEDS);
    return false;
  }
  feeds.push_back(feed);
  return saveToFile();
}

bool RssFeedStore::updateFeed(size_t index, const RssFeed& feed) {
  if (index >= feeds.size()) return false;
  feeds[index] = feed;
  return saveToFile();
}

bool RssFeedStore::removeFeed(size_t index) {
  if (index >= feeds.size()) return false;
  feeds.erase(feeds.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const RssFeed* RssFeedStore::getFeed(size_t index) const {
  if (index >= feeds.size()) return nullptr;
  return &feeds[index];
}
