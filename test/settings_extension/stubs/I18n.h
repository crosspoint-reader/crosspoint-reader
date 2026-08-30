#pragma once

// Minimal stand-in for lib/I18n/I18n.h: the real header pulls in
// I18nKeys.h/I18nStrings.h, which are generated at build time by
// scripts/gen_i18n.py and not present in a host-test build. Only StrId
// (SettingInfo::nameId's type) and the tr()/I18N macros are exercised by
// SettingsTypes.h/SettingsExtension.h, so this stub covers just that.

enum class StrId {
  STR_NONE_OPT,
  STR_TEST_ROW_A,
  STR_TEST_ROW_B,
  // Stands in for any built-in row the host special-cases by nameId
  // (SettingsActivity does exactly this for STR_TIME_TO_SLEEP).
  STR_TEST_HOST_SPECIAL_CASED,
};

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }
  const char* get(StrId) const { return ""; }
};

#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
