#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

/**
 * Singleton storing Koofr WebDAV credentials on the SD card.
 *
 * The password is XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (same scheme as the KOReader and
 * OPDS stores: not cryptographically secure, but it keeps the secret off a
 * plainly readable SD card and ties it to this device).
 *
 * Note: Koofr's WebDAV endpoint rejects the account login password. The user
 * must create an application-specific password in Koofr's web preferences.
 */
class KoofrCredentialStore : public PersistableStore<KoofrCredentialStore> {
 private:
  std::string username;   // Koofr account e-mail
  std::string password;   // Koofr app password (plaintext in memory, obfuscated on disk)
  std::string serverUrl;  // WebDAV base URL override (empty = Koofr's own endpoint)
  std::string remoteDir;  // Destination folder relative to the WebDAV root (empty = default)

  KoofrCredentialStore() = default;
  ~KoofrCredentialStore() = default;

  friend class PersistableStore<KoofrCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/koofr.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }
  bool hasCredentials() const;
  void clearCredentials();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  // WebDAV base URL for requests: the override when set (protocol-normalized),
  // otherwise Koofr's endpoint. Trailing slashes stripped.
  std::string getBaseUrl() const;

  void setRemoteDir(const std::string& dir);
  const std::string& getRemoteDir() const { return remoteDir; }
  // Destination folder with surrounding slashes stripped, falling back to the
  // default. Never empty, so uploads always land in a folder of their own.
  std::string getEffectiveRemoteDir() const;
};

// Helper macro to access the Koofr credential store
#define KOOFR_STORE KoofrCredentialStore::getInstance()
