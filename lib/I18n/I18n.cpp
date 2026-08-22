#include "I18n.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

namespace {
// .cplang layout, little-endian, written by scripts/gen_i18n.py::write_language_packs():
//   magic[8] "CPLANG\0\0" | version u16 | keyCount u16 | blobLen u16 | code[4]
//   offsets u16 * keyCount | blob blobLen
// Offsets carry the same bit-15 "same as English" encoding the compiled tables use, which is
// why English is always built in -- a pack resolves those entries against STRINGS_EN_DATA.
constexpr char PACK_MAGIC[8] = {'C', 'P', 'L', 'A', 'N', 'G', '\0', '\0'};
constexpr uint16_t PACK_VERSION = 1;
constexpr size_t PACK_HEADER_BYTES = 18;

// HalFile::read returns a signed count, so every length check against a sizeof()
// would be a signed/unsigned comparison. One named predicate instead.
bool readExactly(HalFile& file, void* dst, const size_t count) {
  const int got = file.read(dst, count);
  return got > 0 && static_cast<size_t>(got) == count;
}
}  // namespace

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // A loaded pack wins: getLanguageStrings() answers {nullptr, nullptr} for any language whose
  // strings are not compiled in.
  const LangStrings lang = _packStrings.data ? _packStrings : getLanguageStrings(_language);
  if (!lang.data || !lang.offsets) return "???";

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang.offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang.data + off;
}

bool I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return false;
  }
  if (isLanguageBuiltIn(lang)) {
    // Release the previous pack: nothing reads it once a compiled-in language is active, and it
    // is the largest single allocation this class ever makes.
    _packBuffer.reset();
    _packStrings = {nullptr, nullptr};
    _language = lang;
    return true;
  }
  if (!loadPack(lang)) {
    LOG_ERR("I18N", "No usable pack for %s; keeping %s", LANGUAGE_CODES[static_cast<size_t>(lang)],
            LANGUAGE_CODES[static_cast<size_t>(_language)]);
    return false;
  }
  _language = lang;
  return true;
}

bool I18n::loadPack(const Language lang) {
  char path[64];
  snprintf(path, sizeof(path), "%s/%s.cplang", PACK_DIR, LANGUAGE_CODES[static_cast<size_t>(lang)]);

  HalFile file;
  if (!Storage.openFileForRead("I18N", path, file)) return false;

  uint8_t header[PACK_HEADER_BYTES];
  if (!readExactly(file, header, sizeof(header))) return false;
  if (memcmp(header, PACK_MAGIC, sizeof(PACK_MAGIC)) != 0) {
    LOG_ERR("I18N", "%s: not a language pack", path);
    return false;
  }
  uint16_t version = 0;
  uint16_t keyCount = 0;
  uint16_t blobLen = 0;
  memcpy(&version, header + 8, sizeof(version));
  memcpy(&keyCount, header + 10, sizeof(keyCount));
  memcpy(&blobLen, header + 12, sizeof(blobLen));
  if (version != PACK_VERSION) {
    LOG_ERR("I18N", "%s: pack version %u, firmware wants %u", path, version, PACK_VERSION);
    return false;
  }
  // The offsets index the string table by position, so a pack built against a different set of
  // keys would silently return the wrong strings. Refuse it instead.
  if (keyCount != static_cast<uint16_t>(StrId::_COUNT)) {
    LOG_ERR("I18N", "%s: pack has %u keys, firmware has %u -- regenerate the packs", path, keyCount,
            static_cast<unsigned>(StrId::_COUNT));
    return false;
  }

  const size_t offsetBytes = static_cast<size_t>(keyCount) * sizeof(uint16_t);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(offsetBytes + blobLen);
  if (!buffer) {
    LOG_ERR("I18N", "OOM: %u bytes for %s", static_cast<unsigned>(offsetBytes + blobLen), path);
    return false;
  }
  if (!readExactly(file, buffer.get(), offsetBytes + blobLen)) {
    LOG_ERR("I18N", "%s: truncated pack", path);
    return false;
  }

  // Publish only once the whole pack is in hand, so a failed read can never leave get() pointing
  // at a half-filled buffer.
  _packBuffer = std::move(buffer);
  _packStrings = {reinterpret_cast<const char*>(_packBuffer.get() + offsetBytes),
                  reinterpret_cast<const uint16_t*>(_packBuffer.get())};
  LOG_INF("I18N", "Loaded %s pack (%u bytes)", LANGUAGE_CODES[static_cast<size_t>(lang)],
          static_cast<unsigned>(offsetBytes + blobLen));
  return true;
}

bool I18n::isLanguageAvailable(const Language lang) {
  if (lang >= Language::_COUNT) return false;
  if (isLanguageBuiltIn(lang)) return true;
  char path[64];
  snprintf(path, sizeof(path), "%s/%s.cplang", PACK_DIR, LANGUAGE_CODES[static_cast<size_t>(lang)]);
  return Storage.exists(path);
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
