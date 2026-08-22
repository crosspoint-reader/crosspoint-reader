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
  /// Select the UI language. A compiled-in language switches with no I/O. Anything else needs
  /// its .cplang pack on the card, read here into one heap buffer. If the pack is missing or
  /// unusable the language does not change and this returns false.
  bool setLanguage(Language lang);

  /// True when `lang` can be selected right now — compiled in, or its pack is on the card.
  /// The picker uses this to mark the rest.
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
  // Backing store for a language that is not compiled in, held while that language is active.
  // get() runs in the render loop and resolves by pointer; it never touches the card. Null when
  // the active language is built in.
  std::unique_ptr<uint8_t[]> _packBuffer;
  LangStrings _packStrings{nullptr, nullptr};
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
