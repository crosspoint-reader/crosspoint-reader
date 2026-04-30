#include "I18n.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <string>

#include "I18nStrings.h"

using namespace i18n_strings;

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr uint8_t SETTINGS_VERSION = 2;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const LangStrings lang = getLanguageStrings(_language);
  return lang.data + lang.offsets[index];
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  saveSettings();
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

const char* I18n::getLanguageCode(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return LANGUAGE_CODES[0];
  }
  return LANGUAGE_CODES[index];
}

void I18n::saveSettings() {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    LOG_ERR("I18N", "Failed to save settings");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);

  const char* code = getLanguageCode(_language);
  serialization::writeString(file, code);

  LOG_DBG("I18N", "Settings saved: code=%s", code);
}

void I18n::loadSettings() {
  FsFile file;
  if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
    LOG_DBG("I18N", "No settings file, using default");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);

  if (version == SETTINGS_VERSION) {
    std::string code;
    serialization::readString(file, code);

    for (uint8_t i = 0; i < getLanguageCount(); i++) {
      if (code == LANGUAGE_CODES[i]) {
        _language = static_cast<Language>(i);
        LOG_DBG("I18N", "Loaded language: %s", code.c_str());
        return;
      }
    }

    LOG_ERR("I18N", "Unknown language code: %s", code.c_str());
    return;
  }

  if (version == 1) {
    uint8_t lang;
    serialization::readPod(file, lang);
    if (lang < static_cast<size_t>(Language::_COUNT)) {
      _language = static_cast<Language>(lang);
      saveSettings();
      LOG_INF("I18N", "Migrated v1 language setting");
    }
  }
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
