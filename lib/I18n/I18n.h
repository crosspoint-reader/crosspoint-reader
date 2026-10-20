#pragma once

#include <cstdint>

#include "I18nKeys.h"

namespace FlashLangPartition {
class SlotWriter;
}

/// Bytes a language's flash pack carries after its strings, built, erased and mapped with them
/// (FlashLangPartition). CjkUiFonts keeps the active CJK language's UI faces there. One may be
/// registered; without it a pack holds its strings only.
///
/// I18n calls it from refreshActiveStrings(), at boot and on every language change, and only there.
class LanguagePackExtension {
 public:
  /// Bytes the pack of `lang` carries after its strings; 0 for none.
  virtual uint32_t bytes(Language lang) = 0;
  /// Identifies those bytes. Mixed into the pack's stamp, so a firmware whose bytes differ rebuilds
  /// the slot even when the strings did not change.
  virtual uint32_t stamp(Language lang) = 0;
  /// Writes exactly bytes(lang) bytes. False abandons the extension; the pack keeps its strings.
  virtual bool produce(Language lang, FlashLangPartition::SlotWriter& out) = 0;
  /// The extension of `lang` is readable at `data`, `len` bytes, until the next call. nullptr
  /// (len 0): there is none, from now on -- always sent before anything that could unmap it.
  virtual void attach(Language lang, const uint8_t* data, uint32_t len) = 0;

 protected:
  ~LanguagePackExtension() = default;
};

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
  void setLanguage(Language lang);
  const char* getLanguageCode(Language lang) const;
  const char* getLanguageName(Language lang) const;

  void saveSettings();
  void loadSettings();

  /// Register what the language packs carry after their strings. Before loadSettings(), so the
  /// first pack resolved already includes it.
  void setPackExtension(LanguagePackExtension* ext) { _ext = ext; }

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN), _active(englishStrings()) {}

  // Read the stored language code. Does not resolve the strings for it.
  void loadLanguageSetting();

  // Point _active at the strings for _language: English straight from rodata,
  // anything else from the flash language pack — building that pack first if the
  // slot does not already hold this language and firmware build. Falls back to
  // English on any failure, so a missing or unwritable pack degrades to readable
  // English rather than to blank rows.
  void refreshActiveStrings();

  Language _language;

  // The resolved (data, offsets) pair for _language.
  //
  // Cached rather than resolved per lookup: for a pack language that means
  // reading a flash header and comparing a stamp, and tr() is called several
  // times for every row drawn. For English it points at rodata exactly as
  // before. Either way get() is two loads and an add.
  LangStrings _active;

  LanguagePackExtension* _ext = nullptr;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
