#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class FsFile;

/// Compact translation manager for ESP32.
/// Uses a sorted array + string pool instead of std::map for minimal heap fragmentation.
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
  uint16_t getCount() const { return pairCount; }

  /// Approximate heap bytes used by the string pool + index.
  size_t getMemoryUsage() const;

  /// Language info returned by getAvailableLanguages().
  struct LangInfo {
    char code[8];
    char name[64];
  };

  /// Scan SD card for available .lang files.
  /// Returns cached list: English first, then alphabetically by display name.
  /// Each .lang file must contain a `language.name=...` line for its display name.
  const std::vector<LangInfo>& getAvailableLanguages();

  /// Build a vector of display names suitable for SettingInfo::DynamicEnum.
  std::vector<std::string> getAvailableLanguageNames();

  /// Get the index of the current language in getAvailableLanguages().
  uint8_t getCurrentLanguageIndex();

  /// Invalidate the cached language list (e.g. after SD card changes).
  void invalidateLanguageCache() { languagesScanned = false; }

 private:
  TranslationManager();
  ~TranslationManager();

  struct Pair {
    uint16_t keyOffset;
    uint16_t valueOffset;
  };

  // String pool: single contiguous allocation holding all key\0value\0 data.
  char* pool = nullptr;
  size_t poolSize = 0;
  size_t poolUsed = 0;

  // Sorted index for binary search.
  Pair* pairs = nullptr;
  uint16_t pairCount = 0;
  uint16_t pairCapacity = 0;

  char currentLang[8] = "en";

  // Cached language list from SD card scan.
  std::vector<LangInfo> availableLanguages;
  bool languagesScanned = false;

  void freeAll();
  bool ensurePoolCapacity(size_t bytes);
  bool ensurePairCapacity();
  bool putString(const char* key, const char* value);
  int findKey(const char* key) const;
  bool loadFromSD(const char* lang);
  static bool isValidLangCode(const char* lang);
};

/// Convenience macro. Usage: drawText(T("Settings"), x, y);
/// For English, returns the key itself. For other languages, returns the translation.
#define T(key) TranslationManager::getInstance().getString(key)
