#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

/**
 * @brief Base class for persistable singletons using CRTP.
 *
 * Derived classes must provide:
 * - A private default constructor
 * - friend class PersistableStore<Derived>;
 * - static const char* getFilePath();
 * - String toJson() const;
 * - bool fromJson(const String& json);
 */
template <typename T>
class PersistableStore {
 protected:
  PersistableStore() = default;
  ~PersistableStore() = default;

 public:
  // Delete copy constructor and assignment
  PersistableStore(const PersistableStore&) = delete;
  PersistableStore& operator=(const PersistableStore&) = delete;

  static T& getInstance() {
    static T instance;
    return instance;
  }

  bool saveToFile() const {
    Storage.mkdir("/.crosspoint");
    const char* path = T::getFilePath();
    String json = static_cast<const T*>(this)->toJson();
    if (!Storage.writeFile(path, json)) {
      LOG_ERR("PERSIST", "Failed to write %s", path);
      return false;
    }
    return true;
  }

  bool loadFromFile() {
    const char* path = T::getFilePath();
    if (!Storage.exists(path)) {
      return false;  // Expected on first boot — not an error.
    }
    String json = Storage.readFile(path);
    if (json.isEmpty()) {
      LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
      return false;
    }
    return static_cast<T*>(this)->fromJson(json);
  }

 protected:
  /**
   * Helper function for extracting an obfuscated password from a JSON value.
   * Accepts JsonVariantConst so callers can pass either a whole JsonDocument
   * or a JsonObject element (e.g. inside an array iteration).
   * If the decoded password requires a resave (e.g. from plaintext fallback), `needsResave` is set to true.
   */
  std::string extractPassword(JsonVariantConst doc, bool& needsResave) const {
    bool ok = false;
    std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
    if (!ok) {
      // Deobfuscation failed — fall back to legacy plaintext password.
      pass = doc["password"] | std::string("");
      if (!pass.empty()) needsResave = true;
    }
    // A successfully decoded empty string is a legitimate value; preserve as-is.
    return pass;
  }
};
