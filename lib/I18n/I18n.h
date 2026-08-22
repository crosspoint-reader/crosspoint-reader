#pragma once

#include <cstdint>
#include <memory>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }
  /// Select the UI language. A language whose strings are compiled in switches with no I/O; any
  /// other one needs its .cplang pack on the SD card, which is read into a single heap buffer
  /// here. A missing or unusable pack leaves the language unchanged and returns false, so the UI
  /// never ends up pointing at strings that were not loaded.
  bool setLanguage(Language lang);

  /// True when `lang` can be selected right now: either compiled in, or its pack is on the card.
  /// Used by the language picker to mark which entries need a pack download.
  static bool isLanguageAvailable(Language lang);

  /// Directory holding the .cplang packs, alongside the SD font roots.
  static constexpr const char* PACK_DIR = "/.crosspoint/lang";
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN) {}

  // Reads PACK_DIR/<CODE>.cplang into _packBuffer and points _packStrings at it.
  // Leaves both untouched and returns false on any rejection.
  bool loadPack(Language lang);

  Language _language;
  // Backing store for a language that is not compiled in. Held for as long as that language is
  // selected: get() runs in the render loop and must resolve by pointer, never by reading the
  // card. Null whenever the active language is built in.
  std::unique_ptr<uint8_t[]> _packBuffer;
  LangStrings _packStrings{nullptr, nullptr};
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
