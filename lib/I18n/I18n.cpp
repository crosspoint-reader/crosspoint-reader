#include "I18n.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>
#include <set>
#include <string>

#include "I18nStrings.h"

using namespace i18n_strings;

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr uint8_t SETTINGS_VERSION = 1;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  switch (_language) {
    case Language::SPANISH:
      return i18n_strings::STRINGS_ES[index];
    case Language::ITALIAN:
      return i18n_strings::STRINGS_IT[index];
    case Language::SWEDISH:
      return i18n_strings::STRINGS_SV[index];
    case Language::FRENCH:
      return i18n_strings::STRINGS_FR[index];
    case Language::ENGLISH:
    default:
      return i18n_strings::STRINGS_EN[index];
  }
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  saveSettings();
}

void I18n::saveSettings() {
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    Serial.printf("[I18N] Failed to save settings\n");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(_language));

  file.close();
  Serial.printf("[I18N] Settings saved: language=%d\n", static_cast<int>(_language));
}

void I18n::loadSettings() {
  FsFile file;
  if (!SdMan.openFileForRead("I18N", SETTINGS_FILE, file)) {
    Serial.printf("[I18N] No settings file, using default (English)\n");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != SETTINGS_VERSION) {
    Serial.printf("[I18N] Settings version mismatch\n");
    file.close();
    return;
  }

  uint8_t lang;
  serialization::readPod(file, lang);
  if (lang < static_cast<uint8_t>(Language::_COUNT)) {
    _language = static_cast<Language>(lang);
    Serial.printf("[I18N] Loaded language: %d\n", static_cast<int>(_language));
  }

  file.close();
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  static std::string charsetEN;
  static std::string charsetES;
  static std::string charsetIT;
  static std::string charsetSV;
  static std::string charsetFR;

  const char* const* strings;
  std::string* charset;

  switch (lang) {
    case Language::SPANISH:
      strings = i18n_strings::STRINGS_ES;
      charset = &charsetES;
      break;
    case Language::ITALIAN:
      strings = i18n_strings::STRINGS_IT;
      charset = &charsetIT;
      break;
    case Language::SWEDISH:
      strings = i18n_strings::STRINGS_SV;
      charset = &charsetSV;
      break;
    case Language::FRENCH:
      strings = i18n_strings::STRINGS_FR;
      charset = &charsetFR;
      break;
    case Language::ENGLISH:
    default:
      strings = i18n_strings::STRINGS_EN;
      charset = &charsetEN;
      break;
  }

  // Only generate once
  if (!charset->empty()) {
    return charset->c_str();
  }

  std::set<uint32_t> uniqueChars;

  // Iterate through all strings
  for (size_t i = 0; i < static_cast<size_t>(StrId::_COUNT); i++) {
    const char* str = strings[i];
    while (*str) {
      // Decode UTF-8
      uint32_t cp = 0;
      uint8_t b = static_cast<uint8_t>(*str);

      if ((b & 0x80) == 0) {
        // ASCII
        cp = b;
        str++;
      } else if ((b & 0xE0) == 0xC0) {
        // 2-byte UTF-8
        cp = (b & 0x1F) << 6;
        cp |= (static_cast<uint8_t>(str[1]) & 0x3F);
        str += 2;
      } else if ((b & 0xF0) == 0xE0) {
        // 3-byte UTF-8
        cp = (b & 0x0F) << 12;
        cp |= (static_cast<uint8_t>(str[1]) & 0x3F) << 6;
        cp |= (static_cast<uint8_t>(str[2]) & 0x3F);
        str += 3;
      } else if ((b & 0xF8) == 0xF0) {
        // 4-byte UTF-8
        cp = (b & 0x07) << 18;
        cp |= (static_cast<uint8_t>(str[1]) & 0x3F) << 12;
        cp |= (static_cast<uint8_t>(str[2]) & 0x3F) << 6;
        cp |= (static_cast<uint8_t>(str[3]) & 0x3F);
        str += 4;
      } else {
        str++;  // Invalid byte, skip
        continue;
      }

      uniqueChars.insert(cp);
    }
  }

  // Convert to sorted UTF-8 string
  for (uint32_t cp : uniqueChars) {
    if (cp < 0x80) {
      *charset += static_cast<char>(cp);
    } else if (cp < 0x800) {
      *charset += static_cast<char>(0xC0 | (cp >> 6));
      *charset += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      *charset += static_cast<char>(0xE0 | (cp >> 12));
      *charset += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      *charset += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      *charset += static_cast<char>(0xF0 | (cp >> 18));
      *charset += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      *charset += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      *charset += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  return charset->c_str();
}
