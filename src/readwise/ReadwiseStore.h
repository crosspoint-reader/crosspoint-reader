#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Which Readwise Reader locations are mirrored to the device.
enum class ReadwiseSources : uint8_t {
  Later = 0,                // "Later"
  LaterShortlist = 1,       // "Later" + "Shortlist" (default)
  InboxLaterShortlist = 2,  // "Inbox" + "Later" + "Shortlist"
  Count = 3,
};

/**
 * Readwise Reader sync settings, stored in /.crosspoint/readwise.json.
 * The access token is obfuscated the same way as the KOReader password
 * (device-bound XOR + base64): not encryption, but not plain text on the card.
 */
class ReadwiseStore : public PersistableStore<ReadwiseStore> {
 private:
  std::string token;
  ReadwiseSources sources = ReadwiseSources::LaterShortlist;
  bool archiveFinished = true;  // archive in Readwise when finished on device
  bool removeArchived = true;   // delete local copies no longer in the queue
  uint8_t maxArticles = 50;
  std::string folder = "/Readwise";

  ReadwiseStore() = default;
  ~ReadwiseStore() = default;
  friend class PersistableStore<ReadwiseStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/readwise.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::string& getToken() const { return token; }
  void setToken(const std::string& t);
  bool hasToken() const { return !token.empty(); }

  ReadwiseSources getSources() const { return sources; }
  void setSources(ReadwiseSources s) { sources = s; }
  bool getArchiveFinished() const { return archiveFinished; }
  void setArchiveFinished(bool v) { archiveFinished = v; }
  bool getRemoveArchived() const { return removeArchived; }
  void setRemoveArchived(bool v) { removeArchived = v; }
  uint8_t getMaxArticles() const { return maxArticles; }
  const std::string& getFolder() const { return folder; }
  void setFolder(const std::string& f);
};

#define READWISE_STORE ReadwiseStore::getInstance()
