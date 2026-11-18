#pragma once

#include <string>

struct ClippingStore {
  static bool deleteForFilePath(const std::string&, const char*) { return true; }
};
