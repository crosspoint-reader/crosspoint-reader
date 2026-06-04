#pragma once
#include <string>
#include <vector>

struct RssFeed {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

class RssFeedStore;
namespace JsonSettingsIO {
bool saveRss(const RssFeedStore& store, const char* path);
bool loadRss(RssFeedStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

class RssFeedStore {
 private:
  static RssFeedStore instance;
  std::vector<RssFeed> feeds;

  static constexpr size_t MAX_FEEDS = 8;

  RssFeedStore() = default;

  friend bool JsonSettingsIO::saveRss(const RssFeedStore&, const char*);
  friend bool JsonSettingsIO::loadRss(RssFeedStore&, const char*, bool*);

 public:
  RssFeedStore(const RssFeedStore&) = delete;
  RssFeedStore& operator=(const RssFeedStore&) = delete;

  static RssFeedStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addFeed(const RssFeed& feed);
  bool updateFeed(size_t index, const RssFeed& feed);
  bool removeFeed(size_t index);

  const std::vector<RssFeed>& getFeeds() const { return feeds; }
  const RssFeed* getFeed(size_t index) const;
  size_t getCount() const { return feeds.size(); }
  bool hasFeeds() const { return !feeds.empty(); }
};

#define RSS_STORE RssFeedStore::getInstance()
