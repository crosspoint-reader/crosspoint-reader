#include "I18n.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdlib>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

// Settings file — stores language as a null-terminated code string (e.g. "EN", "FI").
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.cfg";
static constexpr uint8_t SETTINGS_VERSION = 2;

// Legacy v1 file (stored uint8_t enum index).
static constexpr const char* LEGACY_SETTINGS_FILE = "/.crosspoint/language.bin";

// Maximum size of a language YAML file we will attempt to load.
static constexpr size_t EXT_LANG_BUF_SIZE = 14000;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

// ---------------------------------------------------------------------------
// String lookup
// ---------------------------------------------------------------------------

const char* I18n::get(StrId id) const {
  const size_t index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) return "???";

  if (_language == Language::EXT_LANG && _extTable[index]) {
    return _extTable[index];
  }

  // Core language (or EN fallback for EXTERNAL when string is missing).
  const Language coreLang = (_language == Language::EXT_LANG) ? Language::EN : _language;
  return getStringArray(coreLang)[index];
}

// ---------------------------------------------------------------------------
// Language switching
// ---------------------------------------------------------------------------

void I18n::setLanguage(Language lang) {
  if (static_cast<uint8_t>(lang) >= static_cast<uint8_t>(Language::_CORE_COUNT)) return;
  unloadExternalLanguage();
  _language = lang;
  saveSettings();
}

bool I18n::setExternalLanguage(const char* code) {
  if (!code || code[0] == '\0') return false;
  if (!loadExternalLanguage(code)) return false;
  memset(_missingExternalCode, 0, sizeof(_missingExternalCode));
  saveSettings();
  return true;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const char* I18n::getActiveCode() const {
  if (_language == Language::EXT_LANG) return _extCode;
  // Core languages occupy the first _CORE_COUNT entries of ALL_LANGUAGES
  // in the same order as the Language enum, so this is a direct index.
  return ALL_LANGUAGES[static_cast<uint8_t>(_language)].code;
}

const char* I18n::getActiveName() const {
  if (_language == Language::EXT_LANG) {
    return _extName[0] ? _extName : _extCode;  // name from YAML metadata, or code as fallback
  }
  return getLanguageName(_language);
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_CORE_COUNT)) return "???";
  return LANGUAGE_NAMES[index];
}

const char* I18n::getCharacterSet() const {
  if (_language == Language::EXT_LANG) return CHARACTER_SETS[0];  // EN fallback
  return getCharacterSet(_language);
}

const char* I18n::getCharacterSet(Language lang) {
  const auto idx = static_cast<size_t>(lang);
  if (idx >= static_cast<size_t>(Language::_CORE_COUNT)) return CHARACTER_SETS[0];
  return CHARACTER_SETS[idx];
}

bool I18n::consumeMissingExternalLanguageCode(char* outCode, size_t outCodeLen) {
  if (!outCode || outCodeLen == 0 || _missingExternalCode[0] == '\0') return false;

  strncpy(outCode, _missingExternalCode, outCodeLen - 1);
  outCode[outCodeLen - 1] = '\0';
  memset(_missingExternalCode, 0, sizeof(_missingExternalCode));
  return true;
}

void I18n::markMissingExternalLanguageCode(const char* code) {
  if (!code || code[0] == '\0') return;
  strncpy(_missingExternalCode, code, sizeof(_missingExternalCode) - 1);
  _missingExternalCode[sizeof(_missingExternalCode) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// External language loader (SD YAML)
// ---------------------------------------------------------------------------

void I18n::unloadExternalLanguage() {
  if (_extBuffer) {
    free(_extBuffer);
    _extBuffer = nullptr;
  }
  memset(_extTable, 0, sizeof(_extTable));
  memset(_extCode, 0, sizeof(_extCode));
  memset(_extName, 0, sizeof(_extName));
  if (_language == Language::EXT_LANG) _language = Language::EN;
}

// Read one '\n'-terminated line from file into buf (strips \r, null-terminates).
// Returns false at EOF with no bytes read.
static bool readFileLine(FsFile& file, char* buf, size_t bufSize) {
  size_t pos = 0;
  while (pos < bufSize - 1) {
    uint8_t c;
    if (file.read(&c, 1) != 1) {
      buf[pos] = '\0';
      return pos > 0;
    }
    if (c == '\n') break;
    if (c != '\r') buf[pos++] = static_cast<char>(c);
  }
  buf[pos] = '\0';

  // Keep parser alignment by consuming the remainder of an oversized line.
  if (pos == bufSize - 1) {
    uint8_t c;
    while (file.read(&c, 1) == 1) {
      if (c == '\n') break;
    }
  }

  return true;
}

bool I18n::loadExternalLanguage(const char* code) {
  char path[48];
  snprintf(path, sizeof(path), "/.crosspoint/languages/%s.yaml", code);

  FsFile file;
  if (!Storage.openFileForRead("I18N", path, file)) {
    LOG_DBG("I18N", "External language file not found: %s", path);
    return false;
  }

  char* tempBuffer = static_cast<char*>(malloc(EXT_LANG_BUF_SIZE));
  if (!tempBuffer) {
    LOG_ERR("I18N", "Out of memory loading external language");
    file.close();
    return false;
  }

  const char* tempTable[static_cast<size_t>(StrId::_COUNT)] = {};
  char tempName[sizeof(_extName)] = {};

  size_t bufUsed = 0;
  int strCount = 0;
  char line[512];

  while (readFileLine(file, line, sizeof(line))) {
    // Skip blank lines; capture _language_name metadata, skip other _ lines.
    if (line[0] == '\0') continue;
    if (line[0] == '_') {
      if (strncmp(line, "_language_name:", 15) == 0) {
        const char* p = line + 15;
        while (*p == ' ') p++;
        if (*p == '"') {
          p++;
          size_t n = 0;
          while (*p && *p != '"' && n < sizeof(tempName) - 1) tempName[n++] = *p++;
          tempName[n] = '\0';
        }
      }
      continue;
    }

    // Expect:  KEY: "value"
    char* colon = strchr(line, ':');
    if (!colon) continue;
    *colon = '\0';
    const char* key = line;

    // Advance past ':' and whitespace to opening quote
    char* p = colon + 1;
    while (*p == ' ') p++;
    if (*p != '"') continue;
    p++;  // skip opening quote

    // Record where this string starts in _extBuffer
    const size_t strStart = bufUsed;

    // Unescape and copy value into _extBuffer
    while (*p && bufUsed < EXT_LANG_BUF_SIZE - 1) {
      if (*p == '"') {
        p++;
        break;
      }  // closing quote
      if (*p == '\\' && *(p + 1)) {
        const char esc = *(p + 1);
        if (esc == 'n') {
          tempBuffer[bufUsed++] = '\n';
          p += 2;
        } else if (esc == '"') {
          tempBuffer[bufUsed++] = '"';
          p += 2;
        } else if (esc == '\\') {
          tempBuffer[bufUsed++] = '\\';
          p += 2;
        } else {
          p++;
        }  // unknown escape: skip
      } else {
        tempBuffer[bufUsed++] = *p++;
      }
    }

    if (bufUsed < EXT_LANG_BUF_SIZE) {
      tempBuffer[bufUsed++] = '\0';
    } else {
      tempBuffer[EXT_LANG_BUF_SIZE - 1] = '\0';
    }

    if (strStart >= EXT_LANG_BUF_SIZE) continue;

    // Map key name -> StrId and store pointer
    const StrId id = strIdFromKey(key);
    if (id != StrId::_COUNT) {
      tempTable[static_cast<size_t>(id)] = tempBuffer + strStart;
      strCount++;
    }
  }

  file.close();

  if (strCount == 0) {
    LOG_ERR("I18N", "No valid strings found in %s", path);
    free(tempBuffer);
    return false;
  }

  // Commit parsed language atomically so failed loads do not destroy current state.
  unloadExternalLanguage();
  _extBuffer = tempBuffer;
  memcpy(_extTable, tempTable, sizeof(_extTable));
  strncpy(_extName, tempName, sizeof(_extName) - 1);
  _extName[sizeof(_extName) - 1] = '\0';

  strncpy(_extCode, code, sizeof(_extCode) - 1);
  _extCode[sizeof(_extCode) - 1] = '\0';
  _language = Language::EXT_LANG;

  LOG_DBG("I18N", "Loaded external language %s: %d/%d strings", code, strCount, static_cast<int>(StrId::_COUNT));
  return true;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

void I18n::saveSettings() {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    LOG_ERR("I18N", "Failed to save language settings");
    return;
  }

  const char* code = getActiveCode();
  serialization::writePod(file, SETTINGS_VERSION);
  // Write code as fixed 8-byte null-padded field
  char codeBuf[8] = {};
  strncpy(codeBuf, code, sizeof(codeBuf) - 1);
  file.write(reinterpret_cast<const uint8_t*>(codeBuf), sizeof(codeBuf));
  file.close();

  LOG_DBG("I18N", "Settings saved: language=%s", code);
}

void I18n::loadSettings() {
  // Try new v2 format first
  {
    FsFile file;
    if (Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
      uint8_t version = 0;
      serialization::readPod(file, version);
      if (version == SETTINGS_VERSION) {
        char codeBuf[8] = {};
        file.read(reinterpret_cast<uint8_t*>(codeBuf), sizeof(codeBuf));
        codeBuf[sizeof(codeBuf) - 1] = '\0';
        file.close();

        // Check if this is a core language.
        for (uint8_t i = 0; i < LANGUAGE_META_COUNT; i++) {
          if (strcmp(ALL_LANGUAGES[i].code, codeBuf) == 0) {
            // Core languages occupy indices 0.._CORE_COUNT-1 in ALL_LANGUAGES
            // in the same order as the Language enum.
            _language = static_cast<Language>(i);
            memset(_missingExternalCode, 0, sizeof(_missingExternalCode));
            LOG_DBG("I18N", "Loaded core language: %s", codeBuf);
            return;
          }
        }
        // Not a core language — attempt to load from SD card.
        if (!loadExternalLanguage(codeBuf)) {
          markMissingExternalLanguageCode(codeBuf);
          LOG_DBG("I18N", "Language %s not on SD, defaulting to EN", codeBuf);
        }
        return;
      }
      file.close();
    }
  }

  // Try legacy v1 format (uint8_t enum index)
  {
    FsFile file;
    if (Storage.openFileForRead("I18N", LEGACY_SETTINGS_FILE, file)) {
      uint8_t version = 0;
      serialization::readPod(file, version);
      if (version == 1) {
        uint8_t langIdx = 0;
        serialization::readPod(file, langIdx);
        file.close();
        // Old firmware stored a flat index across all 17 languages in a different
        // order than the new Language enum.  Map old index -> language code first.
        static const char* const kLegacyIndexToCode[] = {"EN", "ES", "FR", "DE", "CS", "PT", "RU", "SV", "RO",
                                                         "CA", "UK", "BE", "IT", "PL", "FI", "DA", "NL"};
        constexpr uint8_t kLegacyCount = sizeof(kLegacyIndexToCode) / sizeof(kLegacyIndexToCode[0]);
        if (langIdx < kLegacyCount) {
          const char* code = kLegacyIndexToCode[langIdx];
          // Check if it's a core language
          for (uint8_t i = 0; i < LANGUAGE_META_COUNT; i++) {
            if (strcmp(ALL_LANGUAGES[i].code, code) == 0) {
              _language = static_cast<Language>(i);
              memset(_missingExternalCode, 0, sizeof(_missingExternalCode));
              LOG_DBG("I18N", "Migrated legacy language %d -> %s (core)", langIdx, code);
              saveSettings();
              return;
            }
          }
          // Non-core: attempt to load from SD card
          if (loadExternalLanguage(code)) {
            memset(_missingExternalCode, 0, sizeof(_missingExternalCode));
            LOG_DBG("I18N", "Migrated legacy language %d -> %s (external)", langIdx, code);
            saveSettings();
          } else {
            markMissingExternalLanguageCode(code);
            LOG_DBG("I18N", "Legacy language %s not on SD, defaulting to EN", code);
          }
        }
        return;
      }
      file.close();
    }
  }

  LOG_DBG("I18N", "No language settings found, defaulting to EN");
}
