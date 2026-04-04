#include "I18n.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
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
    Serial.printf("[I18N] Failed to save settings\n");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writeString(file, getLanguageCode(_language));

  file.close();
  Serial.printf("[I18N] Settings saved: language=%d code=%s\n", static_cast<int>(_language),
                getLanguageCode(_language));
}

void I18n::loadSettings() {
  FsFile file;
  if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
    Serial.printf("[I18N] No settings file, using default (English)\n");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);

  if (version == SETTINGS_VERSION) {
    std::string code;
    serialization::readString(file, code);
    bool found = false;

    for (uint8_t i = 0; i < getLanguageCount(); i++) {
      if (code == LANGUAGE_CODES[i]) {
        _language = static_cast<Language>(i);
        found = true;
        break;
      }
    }

    if (found) {
      Serial.printf("[I18N] Loaded language code: %s (%d)\n", code.c_str(), static_cast<int>(_language));
    } else {
      Serial.printf("[I18N] Unknown language code in settings: %s\n", code.c_str());
    }
    file.close();
    return;
  }

  // Legacy migration path: version 1 stored language enum index directly.
  if (version == 1) {
    uint8_t lang;
    serialization::readPod(file, lang);
    if (lang < static_cast<size_t>(Language::_COUNT)) {
      _language = static_cast<Language>(lang);
      Serial.printf("[I18N] Migrating v1 language index: %d -> %s\n", static_cast<int>(_language),
                    getLanguageCode(_language));
      file.close();
      saveSettings();
      return;
    }
    file.close();
    Serial.printf("[I18N] Invalid v1 language index: %d\n", static_cast<int>(lang));
    return;
  }

  Serial.printf("[I18N] Settings version mismatch: %d\n", static_cast<int>(version));

  file.close();
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
