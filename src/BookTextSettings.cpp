#include "BookTextSettings.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>
#include <iterator>

namespace {
constexpr uint8_t FORMAT_VERSION = 1;
constexpr uint16_t FAMILY_BIT = 1;
// Order and mask bits are part of text-settings.bin version 1.
constexpr uint8_t ReaderTextSettings::* SCALAR_FIELDS[] = {
    &ReaderTextSettings::fontPointSize,         &ReaderTextSettings::lineSpacing,
    &ReaderTextSettings::wordSpacing,           &ReaderTextSettings::characterSpacing,
    &ReaderTextSettings::extraParagraphSpacing, &ReaderTextSettings::paragraphIndentSpaces,
    &ReaderTextSettings::paragraphAlignment,    &ReaderTextSettings::screenMargin,
    &ReaderTextSettings::focusReadingEnabled,   &ReaderTextSettings::hyphenationEnabled,
    &ReaderTextSettings::embeddedStyle,         &ReaderTextSettings::textAntiAliasing};
constexpr uint16_t ALL_FIELDS = (1U << (std::size(SCALAR_FIELDS) + 1)) - 1;
constexpr size_t RECORD_SIZE = sizeof(uint8_t) + sizeof(uint16_t) + 32 + std::size(SCALAR_FIELDS);

ReaderTextSettings overlay(ReaderTextSettings base, const ReaderTextSettings& values, const uint16_t mask) {
  if (mask & FAMILY_BIT) std::memcpy(base.sdFontFamilyName, values.sdFontFamilyName, sizeof(base.sdFontFamilyName));
  for (size_t i = 0; i < std::size(SCALAR_FIELDS); ++i) {
    if (mask & (1U << (i + 1))) base.*SCALAR_FIELDS[i] = values.*SCALAR_FIELDS[i];
  }
  return base;
}
}  // namespace

void BookTextSettings::load(const std::string& path) {
  cachePath = path;
  overrideMask = 0;
  overrides = ReaderTextSettings{};
  if (cachePath.empty()) return;
  const std::string filePath = cachePath + "/text-settings.bin";
  if (!Storage.exists(filePath.c_str())) return;
  HalFile file;
  if (!Storage.openFileForRead("TXT", filePath, file)) {
    LOG_ERR("TXT", "Could not open book text settings");
    return;
  }
  uint8_t version = 0;
  uint16_t mask = 0;
  ReaderTextSettings values;
  bool valid = file.fileSize() == RECORD_SIZE && serialization::tryReadPod(file, version) &&
               version == FORMAT_VERSION && serialization::tryReadPod(file, mask) && !(mask & ~ALL_FIELDS) &&
               file.read(values.sdFontFamilyName, sizeof(values.sdFontFamilyName)) ==
                   static_cast<int>(sizeof(values.sdFontFamilyName));
  for (const auto field : SCALAR_FIELDS) {
    if (valid) valid = serialization::tryReadPod(file, values.*field);
  }
  if (!valid || !overlay(ReaderTextSettings{}, values, mask).isValid()) {
    LOG_ERR("TXT", "Invalid book text settings: %s", filePath.c_str());
    return;
  }
  overrides = values;
  overrideMask = mask;
}

ReaderTextSettings BookTextSettings::resolve() const {
  return overlay(ReaderTextSettings::defaults(), overrides, overrideMask);
}

bool BookTextSettings::update(const ReaderTextSettings& edited) {
  if (!edited.isValid()) {
    LOG_ERR("TXT", "Invalid edited book text settings");
    return false;
  }
  const auto current = resolve();
  if (edited == current) return true;
  const auto defaults = ReaderTextSettings::defaults();
  auto candidate = overrides;
  uint16_t mask = overrideMask;
  if (std::strncmp(edited.sdFontFamilyName, current.sdFontFamilyName, sizeof(edited.sdFontFamilyName)) != 0) {
    std::memcpy(candidate.sdFontFamilyName, edited.sdFontFamilyName, sizeof(candidate.sdFontFamilyName));
    if (std::strncmp(edited.sdFontFamilyName, defaults.sdFontFamilyName, sizeof(edited.sdFontFamilyName)) == 0) {
      mask &= ~FAMILY_BIT;
    } else {
      mask |= FAMILY_BIT;
    }
  }
  for (size_t i = 0; i < std::size(SCALAR_FIELDS); ++i) {
    const auto field = SCALAR_FIELDS[i];
    if (edited.*field == current.*field) continue;
    candidate.*field = edited.*field;
    const uint16_t bit = 1U << (i + 1);
    if (edited.*field == defaults.*field) {
      mask &= ~bit;
    } else {
      mask |= bit;
    }
  }
  if (!save(candidate, mask)) return false;
  overrides = candidate;
  overrideMask = mask;
  return true;
}

bool BookTextSettings::reset() {
  if (!save(ReaderTextSettings{}, 0)) return false;
  overrides = ReaderTextSettings{};
  overrideMask = 0;
  return true;
}

bool BookTextSettings::save(const ReaderTextSettings& values, const uint16_t mask) const {
  if (cachePath.empty()) {
    LOG_ERR("TXT", "Book text settings have no cache path");
    return false;
  }
  const std::string filePath = cachePath + "/text-settings.bin";
  if (mask == 0) {
    if (!Storage.exists(filePath.c_str()) || Storage.remove(filePath.c_str())) return true;
    LOG_ERR("TXT", "Could not remove book text settings");
    return false;
  }
  if (!Storage.ensureDirectoryExists(cachePath.c_str())) {
    LOG_ERR("TXT", "Could not create book cache for text settings");
    return false;
  }
  const std::string tmpPath = filePath + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("TXT", tmpPath, file)) {
    LOG_ERR("TXT", "Could not open temporary book text settings");
    return false;
  }
  bool written =
      serialization::tryWritePod(file, FORMAT_VERSION) && serialization::tryWritePod(file, mask) &&
      file.write(values.sdFontFamilyName, sizeof(values.sdFontFamilyName)) == sizeof(values.sdFontFamilyName);
  for (const uint8_t ReaderTextSettings::* const field : SCALAR_FIELDS) {
    if (written) written = serialization::tryWritePod(file, values.*field);
  }
  file.flush();
  const bool closed = file.close();
  if (!written || !closed || !Storage.replaceFile(tmpPath.c_str(), filePath.c_str())) {
    LOG_ERR("TXT", "Could not save book text settings");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
