#pragma once

#include <I18n.h>

#include "CrossPointSettings.h"

namespace home_button {
inline constexpr StrId ACTION_LABELS[] = {StrId::STR_HOME_SHORTCUT, StrId::STR_IGNORE,           StrId::STR_PAGE_TURN,
                                          StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES,        StrId::STR_CONFIRM,
                                          StrId::STR_KOSYNC,        StrId::STR_BOOKMARK_OPTION,  StrId::STR_DICTIONARY,
                                          StrId::STR_READER_MENU,   StrId::STR_TOGGLE_FRONTLIGHT};
static_assert(sizeof(ACTION_LABELS) / sizeof(ACTION_LABELS[0]) == static_cast<unsigned>(HomeButtonAction::Count));
inline constexpr StrId GESTURE_LABELS[] = {StrId::STR_HOME_BUTTON_TAP, StrId::STR_HOME_BUTTON_DOUBLE_TAP,
                                           StrId::STR_HOME_BUTTON_LONG_PRESS};
inline constexpr uint8_t CrossPointSettings::* FIELDS[] = {&CrossPointSettings::homeButtonTapAction,
                                                           &CrossPointSettings::homeButtonDoubleTapAction,
                                                           &CrossPointSettings::homeButtonLongPressAction};
inline constexpr const char* KEYS[] = {"homeButtonTapAction", "homeButtonDoubleTapAction", "homeButtonLongPressAction"};
inline bool isSetting(uint8_t CrossPointSettings::* field) {
  return field == FIELDS[0] || field == FIELDS[1] || field == FIELDS[2];
}
}  // namespace home_button
