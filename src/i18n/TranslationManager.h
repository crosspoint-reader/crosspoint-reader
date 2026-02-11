#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class FsFile;

/// Compact translation manager for ESP32.
/// Uses a flash-resident hash table for O(1) key→ID lookup,
/// and a values-only RAM pool (keys are NOT stored in RAM).
/// English is the implicit fallback: T("key") returns "key" itself when no translation is found.
/// Additional languages load from SD card .lang files in key=value format.
class TranslationManager {
 public:
  TranslationManager(const TranslationManager&) = delete;
  TranslationManager& operator=(const TranslationManager&) = delete;

  static TranslationManager& getInstance();

  /// Initialize with a language code.
  /// For "en", no file is loaded (English keys are their own values).
  /// For other languages, loads /config/lang/<lang>.lang from SD card.
  /// @param lang ISO 639-1 code (e.g. "en", "cs", "de"). Max 7 chars.
  /// @return true if the requested language loaded successfully (or lang=="en").
  bool init(const char* lang = "en");

  /// Look up a translated string by key.
  /// @return Translated string, or the key itself if not found (English fallback).
  const char* getString(const char* key) const;

  /// Current language code.
  const char* getCurrentLanguage() const { return currentLang; }

  /// Number of translations currently loaded.
  uint16_t getCount() const { return loadedCount; }

  /// Approximate heap bytes used by the values pool + offset table.
  size_t getMemoryUsage() const;

  /// Language info returned by getAvailableLanguages().
  struct LangInfo {
    char code[8];
    char name[64];
  };

  /// Scan SD card for available .lang files.
  /// Returns cached list: English first, then alphabetically by display name.
  const std::vector<LangInfo>& getAvailableLanguages();

  /// Build a vector of display names suitable for SettingInfo::DynamicEnum.
  std::vector<std::string> getAvailableLanguageNames();

  /// Get the index of the current language in getAvailableLanguages().
  uint8_t getCurrentLanguageIndex();

  /// Invalidate the cached language list (e.g. after SD card changes).
  void invalidateLanguageCache() { languagesScanned = false; }

  /// FNV-1a 32-bit hash. Must match the Python implementation in lang_compile.py.
  static inline uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
      h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    }
    return h;
  }

 private:
  TranslationManager();
  ~TranslationManager();

  /// Look up a key hash in the flash hash table.
  /// @return Key ID (0..LANG_KEY_COUNT-1), or -1 if not found.
  static int16_t lookupId(uint32_t hash);

  // Values pool: only translated strings (no keys).
  // Allocated only for non-English languages.
  char* pool = nullptr;
  size_t poolSize = 0;
  size_t poolUsed = 0;

  // Offset table: valueOffsets[ID] = offset into pool.
  // 0xFFFF means no translation loaded for this ID (English fallback).
  uint16_t* valueOffsets = nullptr;

  // Number of translations actually loaded.
  uint16_t loadedCount = 0;

  char currentLang[8] = "en";

  // Cached language list from SD card scan.
  std::vector<LangInfo> availableLanguages;
  bool languagesScanned = false;

  void freeAll();
  bool loadFromSD(const char* lang);
  static bool isValidLangCode(const char* lang);
};

/// Convenience macro. Usage: drawText(T("Settings"), x, y);
/// For English, returns the key itself. For other languages, returns the translation.
#define T(key) TranslationManager::getInstance().getString(key)
