#pragma once
// I18n mock — returns the key name as the translation
#include <string>

enum StrId {
  STR_NONE_OPT, STR_BACK, STR_SELECT, STR_CONFIRM, STR_CANCEL, STR_EXIT,
  STR_DIR_UP, STR_DIR_DOWN, STR_DIR_LEFT, STR_DIR_RIGHT,
  STR_DICE_ROLLER, STR_SELECT_DICE, STR_ROLL, STR_REROLL, STR_ROLLING,
  STR_MORSE_CODE, STR_ENCODE_TEXT, STR_DECODE_MORSE, STR_REFERENCE_CHART,
  STR_UNIT_CONVERTER, STR_APPS, STR_GAMES, STR_UTILITIES,
  STR_NETWORK_TOOLS, STR_WIRELESS_TESTING,
  STR_STATE_ON, STR_STATE_OFF, STR_DONE, STR_OK_BUTTON,
  // add more as needed — tests just need them to exist
  STR_MAX
};

struct I18nHelper {
  const char* get(StrId) { return "STR"; }
  void setLanguage(int) {}
  int getLanguage() { return 0; }
  std::string getLanguageName(int) { return "English"; }
};

inline I18nHelper I18N;
inline const char* tr(StrId) { return "str"; }
