#pragma once
#include <string>

class BookFusionTokenStore;
namespace BookFusionJsonIO {
bool save(const BookFusionTokenStore& store, const char* path);
bool load(BookFusionTokenStore& store, const char* json);
}  // namespace BookFusionJsonIO

/**
 * Singleton for storing the BookFusion OAuth Bearer token on the SD card.
 * Token is XOR-obfuscated with the device's hardware MAC address and base64-encoded
 * before writing to JSON (not cryptographically secure, but device-tied).
 *
 * File: /.crosspoint/bookfusion.json
 */
class BookFusionTokenStore {
 public:
  static BookFusionTokenStore& getInstance() { return instance; }

  BookFusionTokenStore(const BookFusionTokenStore&) = delete;
  BookFusionTokenStore& operator=(const BookFusionTokenStore&) = delete;

  bool saveToFile() const;
  bool loadFromFile();

  void setToken(const std::string& token);
  const std::string& getToken() const { return accessToken; }
  bool hasToken() const { return !accessToken.empty(); }
  void clearToken();

  friend bool BookFusionJsonIO::save(const BookFusionTokenStore&, const char*);
  friend bool BookFusionJsonIO::load(BookFusionTokenStore&, const char*);

 private:
  static BookFusionTokenStore instance;
  BookFusionTokenStore() = default;

  std::string accessToken;
};

#define BF_TOKEN_STORE BookFusionTokenStore::getInstance()
