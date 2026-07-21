#pragma once

#ifdef ARDUINOJSON_VERSION_MAJOR
#include <ArduinoJson.h>

#include "WString.h"

namespace ArduinoJson {
template <> struct Converter<String> {
  static void toJson(const String &src, JsonVariant dst) {
    dst.set(src.c_str());
  }
  static String fromJson(JsonVariantConst src) {
    if (src.is<const char *>()) {
      return String(src.as<const char *>());
    }
    return String();
  }
  static bool checkJson(JsonVariantConst src) {
    return src.is<const char *>() || src.is<std::string>();
  }
};
} // namespace ArduinoJson
#endif