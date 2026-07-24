#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AtomicFile.h>

#include <string>

/**
 * @brief Non-template core of PersistableStore.
 *
 * All ArduinoJson parse/serialize machinery is instantiated once here (in
 * PersistableStore.cpp) instead of in every store's translation unit. GCC
 * emits the JSON serializer/parser templates as local .isra clones per TU
 * (~0.5KB each), so keeping serializeJson/deserializeJson out of the stores
 * is what makes the abstraction flash-neutral.
 */
class PersistableStoreBase {
 protected:
  PersistableStoreBase() = default;
  ~PersistableStoreBase() = default;

  // Serializes doc and writes it to path (ensures /.crosspoint exists). Logs on failure.
  static bool writeDocToFile(const char* path, const JsonDocument& doc);

  // Recovers and parses path into doc. Missing is expected on first boot;
  // malformed, oversize and I/O failures remain distinguishable so callers
  // can block a later write from replacing unreadable user data.
  static AtomicFile::LoadStatus readDocFromFile(const char* path, JsonDocument& doc);

  mutable bool persistenceWritable = true;

  /**
   * Helper function for extracting an obfuscated password from a JSON value.
   * Accepts JsonVariantConst so callers can pass either a whole JsonDocument
   * or a JsonObject element (e.g. inside an array iteration).
   * If the decoded password requires a resave (e.g. from plaintext fallback), `needsResave` is set to true.
   */
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave);
};

/**
 * @brief Base class for persistable singletons using CRTP.
 *
 * Derived classes must provide:
 * - A private default constructor
 * - friend class PersistableStore<Derived>;
 * - static const char* getFilePath();
 * - void toJson(JsonDocument& doc) const;
 * - bool fromJson(JsonVariantConst doc);
 *
 * Note for implementers: read string values as `const char*` (e.g.
 * `obj["name"] | ""`), never as `| std::string("")` — ArduinoJson's
 * std::string converter drags a per-TU copy of the whole JSON serializer
 * into flash via its serializeJson fallback.
 */
template <typename T>
class PersistableStore : public PersistableStoreBase {
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

  void markReadOnlyForRecovery() { persistenceWritable = false; }
  bool isPersistenceWritable() const { return persistenceWritable; }

  bool saveToFile() const {
    if (!persistenceWritable) return false;
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }

  bool loadFromFile() {
    JsonDocument doc;
    const AtomicFile::LoadStatus loaded = readDocFromFile(T::getFilePath(), doc);
    if (loaded == AtomicFile::LoadStatus::Missing) {
      persistenceWritable = true;
      return false;
    }
    if (loaded != AtomicFile::LoadStatus::Primary && loaded != AtomicFile::LoadStatus::Backup &&
        loaded != AtomicFile::LoadStatus::Temp) {
      persistenceWritable = false;
      return false;
    }
    persistenceWritable = true;
    const bool parsed = static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
    if (!parsed) persistenceWritable = false;
    return parsed;
  }
};
