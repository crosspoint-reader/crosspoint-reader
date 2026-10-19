#include "I18n.h"

#include <FlashLangPartition.h>
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

  // If bit 15 of the offset is set, apply the offset to the English lookup
  // table. That rule is identical whether _active came from rodata or from the
  // mmap'd language pack — FlashLangPartition writes English offsets into the
  // table it derives for exactly that reason.
  const uint16_t off = _active.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return _active.data + off;
}

void I18n::refreshActiveStrings() {
  // Drop to English FIRST: _active may point into the mapping that build()
  // below is about to tear down, and a stale pointer here is a crash in every
  // drawn row. The extension's reader lets go of it for the same reason.
  _active = englishStrings();
  if (_ext) _ext->attach(_language, nullptr, 0);

  const auto index = static_cast<size_t>(_language);
  if (index == 0 || index >= static_cast<size_t>(Language::_COUNT)) return;

  const i18n_strings::LangPack& pack = i18n_strings::LANGPACKS[index];
  if (!pack.data || pack.compressedLen == 0) return;

  const auto langIndex = static_cast<uint8_t>(index);
  const auto keyCount = static_cast<uint16_t>(StrId::_COUNT);

  // What the slot must hold for this language: its strings, and whatever the extension adds. The
  // extension's stamp is folded in so new font data rebuilds the slot as new strings do.
  const uint32_t extBytes = _ext ? _ext->bytes(_language) : 0;
  const uint32_t stamp = extBytes ? pack.stamp ^ _ext->stamp(_language) : pack.stamp;

  // Built when the slot does not hold this pack -- a different language, the same one from a
  // different firmware build (the stamp covers both), or bytes that changed behind its header since,
  // which acquire() finds by the slot's CRCs and rebuilds at once, in this boot. Rebuilding costs an
  // erase plus ~30 KB of inflate; a CJK language adds its UI faces, ~45-73 KB of inflate more.
  struct Producer {
    LanguagePackExtension* ext;
    Language lang;
  } producer{_ext, _language};
  const FlashLangPartition::Extension extension{extBytes,
                                                [](void* ctx, FlashLangPartition::SlotWriter& out) {
                                                  const auto* p = static_cast<const Producer*>(ctx);
                                                  return p->ext->produce(p->lang, out);
                                                },
                                                &producer};
  FlashLangPartition::Mapped mapped;
  if (!FlashLangPartition::acquire(langIndex, stamp, pack.data, pack.compressedLen, pack.uncompressedLen, keyCount,
                                   OFFSETS_EN, extBytes ? &extension : nullptr, &mapped)) {
    LOG_ERR("I18N", "language pack %u could not be built or mapped - staying on English", langIndex);
    return;
  }

  _active = {mapped.data, mapped.offsets};
  LOG_INF("I18N", "language pack %u active", langIndex);

  // An extension that failed to build, to map or to verify is simply not attached; its reader works
  // without it. One that failed to build or to verify is built again by the next refresh; one the
  // MMU had no page for is not, since a rebuild would not change that.
  if (_ext && mapped.ext && mapped.extLen == extBytes) _ext->attach(_language, mapped.ext, mapped.extLen);
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  refreshActiveStrings();
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
  serialization::writeString(file, getLanguageCode(_language));

  file.close();
  LOG_INF("I18N", "Settings saved: language=%d code=%s", static_cast<int>(_language), getLanguageCode(_language));
}

void I18n::loadSettings() {
  loadLanguageSetting();
  // Separate step because loadLanguageSetting() has several early exits and the
  // pack has to be resolved on every one of them, including the "no settings
  // file" path that leaves the language at English.
  refreshActiveStrings();
}

void I18n::loadLanguageSetting() {
  FsFile file;
  if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
    LOG_INF("I18N", "No settings file, using default (English)");
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
      LOG_INF("I18N", "Loaded language code: %s (%d)", code.c_str(), static_cast<int>(_language));
    } else {
      LOG_ERR("I18N", "Unknown language code in settings: %s", code.c_str());
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
      LOG_INF("I18N", "Migrating v1 language index: %d -> %s", static_cast<int>(_language), getLanguageCode(_language));
      file.close();
      saveSettings();
      return;
    }
    file.close();
    LOG_ERR("I18N", "Invalid v1 language index: %d", static_cast<int>(lang));
    return;
  }

  LOG_ERR("I18N", "Settings version mismatch: %d\n", static_cast<int>(version));

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
