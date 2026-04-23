#pragma once

#include <string>

class WriterSimInput {
 public:
  static void setActive(bool active);
  static bool readAvailable(std::string& out);
};
