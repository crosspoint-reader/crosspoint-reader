#pragma once

#include <map>
#include <string>
#include <vector>

// Forward declaration for FS file or stream if needed,
// but for now we'll take a string buffer or filename to keep it generic?
// Or better, depend on FS.h to read files directly.

#ifdef FILE_READ
#undef FILE_READ
#endif
#ifdef FILE_WRITE
#undef FILE_WRITE
#endif
#include <FS.h>

namespace ThemeEngine {

struct IniSection {
  std::string name;
  std::map<std::string, std::string> properties;
};

class IniParser {
public:
  // Parse a stream (File, Serial, etc.)
  static std::map<std::string, std::map<std::string, std::string>>
  parse(Stream &stream);

  // Parse a string buffer (useful for testing)
  static std::map<std::string, std::map<std::string, std::string>>
  parseString(const std::string &content);

private:
  static void trim(std::string &s);
};

} // namespace ThemeEngine
