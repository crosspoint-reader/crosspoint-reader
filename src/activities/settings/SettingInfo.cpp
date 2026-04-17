#include "SettingInfo.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

std::string SettingInfo::getTitle() const {
  const auto t = I18N.get(nameId);
  return isSeparator ? UITheme::makeSeparatorTitle(t) : t;
}

std::string SettingInfo::getDisplayValue() const {
  if (isSeparator) return {};

  switch (type) {
    case SettingType::TOGGLE: {
      bool value;
      if (valuePtr)
        value = SETTINGS.*(valuePtr);
      else if (valueGetter)
        value = callValueGetter();
      else
        return {};
      return std::string(value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
    }
    case SettingType::ENUM: {
      uint8_t value;
      if (valuePtr)
        value = SETTINGS.*(valuePtr);
      else if (valueGetter)
        value = callValueGetter();
      else
        return {};
      if (value < enumValues.size()) return std::string(I18N.get(enumValues[value]));
      return {};
    }
    case SettingType::VALUE: {
      if (valuePtr) return std::to_string(SETTINGS.*(valuePtr));
      if (valueGetter) return std::to_string(callValueGetter());
      return {};
    }
    case SettingType::ACTION:
      return std::string(">>");
    case SettingType::STRING:
      return {};
  }
  return {};
}

void SettingInfo::toggleValue() const {
  if (isSeparator) return;

  switch (type) {
    case SettingType::TOGGLE:
      if (valuePtr) {
        SETTINGS.*(valuePtr) = !(SETTINGS.*(valuePtr));
      } else if (valueGetter && valueSetter) {
        callValueSetter(!callValueGetter());
      }
      break;

    case SettingType::ENUM: {
      const auto count = static_cast<uint8_t>(enumValues.size());
      if (count == 0) break;
      if (valuePtr) {
        SETTINGS.*(valuePtr) = (SETTINGS.*(valuePtr) + 1) % count;
      } else if (valueGetter && valueSetter) {
        callValueSetter((callValueGetter() + 1) % count);
      }
      break;
    }

    case SettingType::VALUE:
      if (valuePtr) {
        const unsigned current = SETTINGS.*(valuePtr);
        SETTINGS.*(valuePtr) = static_cast<uint8_t>(
            (current + valueRange.step > valueRange.max) ? valueRange.min : current + valueRange.step);
      }
      break;

    default:
      break;
  }
}
