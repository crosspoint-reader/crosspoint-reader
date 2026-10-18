#include "SettingsByKey.h"

#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontGlobals.h"
#include "SettingsList.h"

namespace SettingsByKey {

namespace {

bool isFontFamilyKey(const char* key) {
  return key && (std::strcmp(key, "fontFamily") == 0 || std::strcmp(key, "txtFontFamily") == 0);
}

void store(const SettingInfo& s, const uint8_t value) {
  if (s.valuePtr) {
    SETTINGS.*(s.valuePtr) = value;
  } else if (s.valueSetter) {
    s.callValueSetter(value);
  } else if (s.persistPtr) {
    SETTINGS.*(s.persistPtr) = value;
  }
}

}  // namespace

bool readNumber(const SettingInfo& s, int& out) {
  switch (s.type) {
    case SettingType::TOGGLE:
    case SettingType::ENUM:
    case SettingType::VALUE:
    case SettingType::ACTION:
      if (s.valuePtr) {
        out = SETTINGS.*(s.valuePtr);
      } else if (s.valueGetter) {
        out = s.callValueGetter();
      } else if (s.persistPtr) {
        out = SETTINGS.*(s.persistPtr);
      } else {
        return false;
      }
      return true;
    case SettingType::STRING:
      return false;
  }
  return false;
}

uint8_t optionCount(const SettingInfo& s) {
  // The family rows' option lists are empty by design (the web page fills them in when it serves
  // the row), so count the families themselves.
  return isFontFamilyKey(s.key) ? fontFamilyOptionCount() : s.getEnumOptionCount();
}

bool optionsKnown(const SettingInfo& s) { return !isFontFamilyKey(s.key) || fontFamilyOptionsKnown(); }

std::string optionLabel(const SettingInfo& s, const uint8_t index) {
  return isFontFamilyKey(s.key) ? fontFamilyOptionLabel(index) : s.getEnumOptionLabel(index);
}

WriteResult writeNumber(const SettingInfo& s, const int value) {
  switch (s.type) {
    case SettingType::TOGGLE:
      store(s, value ? 1 : 0);
      return WriteResult::Applied;
    case SettingType::ENUM:
      if (value < 0 || value >= optionCount(s)) return WriteResult::OutOfRange;
      store(s, static_cast<uint8_t>(value));
      return WriteResult::Applied;
    case SettingType::VALUE:
      if (value < s.valueRange.min || value > s.valueRange.max) return WriteResult::OutOfRange;
      store(s, static_cast<uint8_t>(value));
      return WriteResult::Applied;
    case SettingType::ACTION:
      // A slider row: the screen edits it through a picker, and the value lives in persistPtr.
      if (!s.persistPtr) return WriteResult::NotWritable;
      if (value < 0 || value > s.persistMax) return WriteResult::OutOfRange;
      SETTINGS.*(s.persistPtr) = static_cast<uint8_t>(value);
      return WriteResult::Applied;
    case SettingType::STRING:
      return WriteResult::NotWritable;
  }
  return WriteResult::NotWritable;
}

WriteResult writeString(const SettingInfo& s, const std::string& value) {
  if (s.type != SettingType::STRING) return WriteResult::NotWritable;
  if (s.stringSetter) {
    s.callStringSetter(value);
  } else if (s.stringMaxLen > 0) {
    char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
    strncpy(ptr, value.c_str(), s.stringMaxLen - 1);
    ptr[s.stringMaxLen - 1] = '\0';
  } else {
    return WriteResult::NotWritable;
  }
  return WriteResult::Applied;
}

void commit() {
  CrossPointSettings::normalizeDependentSettings(SETTINGS);
  SETTINGS.saveToFile();
}

int visitKeyed(const std::string_view key, const RowVisitor visit, void* ctx) {
  int count = 0;
  forEachSetting([&](const SettingInfo& s) {
    if (!s.key || (!key.empty() && key != s.key)) return;
    visit(s, ctx);
    count++;
  });
  return count;
}

}  // namespace SettingsByKey
