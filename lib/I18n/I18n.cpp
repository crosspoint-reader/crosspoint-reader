#include "I18n.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <InflateReader.h>
#include <Serialization.h>

#include "I18nStrings.h"

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr uint8_t SETTINGS_VERSION = 1;

// Static storage for the active language's decompressed strings.
char I18n::_stringBuffer[I18N_MAX_DECOMPRESSED_SIZE] = {};
const char* I18n::_stringTable[static_cast<size_t>(StrId::_COUNT)] = {};

I18n& I18n::getInstance() {
  static I18n instance;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    instance.decompressLanguage(Language::EN);
  }
  return instance;
}

void I18n::decompressLanguage(Language lang) {
  const auto idx = static_cast<size_t>(lang);
  if (idx >= static_cast<size_t>(Language::_COUNT)) return;

  const auto& data = i18n_strings::LANG_DATA[idx];

  InflateReader reader;
  if (!reader.init(false)) {
    Serial.printf("[I18N] Failed to init decompressor\n");
    return;
  }
  reader.setSource(data.compressed, data.compressedLen);
  reader.skipZlibHeader();

  if (!reader.read(reinterpret_cast<uint8_t*>(_stringBuffer), data.decompressedLen)) {
    Serial.printf("[I18N] Decompression failed for language %d\n", static_cast<int>(lang));
    reader.deinit();
    return;
  }
  reader.deinit();

  // Build string table by scanning null terminators in the flat blob.
  const size_t count = static_cast<size_t>(StrId::_COUNT);
  size_t strIdx = 0;
  size_t pos = 0;
  while (strIdx < count && pos < static_cast<size_t>(data.decompressedLen)) {
    _stringTable[strIdx++] = &_stringBuffer[pos];
    while (pos < static_cast<size_t>(data.decompressedLen) && _stringBuffer[pos] != '\0') ++pos;
    ++pos;  // skip null terminator
  }

  if (strIdx != count) {
    Serial.printf("[I18N] String count mismatch: got %zu, expected %zu\n", strIdx, count);
  }
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }
  const char* s = _stringTable[index];
  return s ? s : "???";
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  decompressLanguage(lang);
  saveSettings();
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

void I18n::saveSettings() {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
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
  if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
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
  if (lang < static_cast<size_t>(Language::_COUNT)) {
    _language = static_cast<Language>(lang);
    Serial.printf("[I18N] Loaded language: %d\n", static_cast<int>(_language));
    decompressLanguage(_language);
  }

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
