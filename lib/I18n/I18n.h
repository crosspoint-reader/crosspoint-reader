#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nKeys.h"

/**
 * Internationalization (i18n) system for CrossPoint Reader.
 *
 * Core languages (EN, DE, FR, ES, PT) are baked into flash — get() returns a
 * direct flash pointer at zero RAM cost.
 *
 * Non-core languages are loaded on demand from SD card
 * (/.crosspoint/languages/XX.yaml).  Missing strings fall back to English.
 */
class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID.
  // For external languages, falls back per-string to English if missing.
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }

  // Set a core language and persist.
  void setLanguage(Language lang);

  // Load a non-core language from /.crosspoint/languages/XX.yaml and persist.
  // Returns false if the file is missing or unparseable (language stays unchanged).
  bool setExternalLanguage(const char* code);

  // Two-letter code of the active language (works for both core and external).
  const char* getActiveCode() const;

  // Display name of the active language.
  const char* getActiveName() const;

  // Display name for a core language by enum value.
  const char* getLanguageName(Language lang) const;

  void saveSettings();
  void loadSettings();

  // Returns true once when a saved external language code was missing on SD.
  // The code is copied into outCode and the pending state is cleared.
  bool consumeMissingExternalLanguageCode(char* outCode, size_t outCodeLen);

  // Character set for the active language (used by font renderer).
  // Returns EN character set for external languages.
  const char* getCharacterSet() const;

  // Character set for a specific core language.
  static const char* getCharacterSet(Language lang);

 private:
  I18n() = default;

  // Parse /.crosspoint/languages/XX.yaml into _extBuffer / _extTable.
  bool loadExternalLanguage(const char* code);
  void unloadExternalLanguage();
  void markMissingExternalLanguageCode(const char* code);

  Language _language = Language::EN;
  char _extCode[8] = {};                                           // active external language code, e.g. "FI"
  char _extName[48] = {};                                          // active external language native name, e.g. "Suomi"
  char _missingExternalCode[8] = {};                               // saved external code missing on boot
  char* _extBuffer = nullptr;                                      // heap-allocated string data
  const char* _extTable[static_cast<size_t>(StrId::_COUNT)] = {};  // ptrs into _extBuffer
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
