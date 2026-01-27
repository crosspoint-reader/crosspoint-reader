#include "IniParser.h"

#include <sstream>

namespace ThemeEngine {

void IniParser::trim(std::string& s) {
  if (s.empty()) return;

  // Trim left
  size_t first = s.find_first_not_of(" \t\n\r");
  if (first == std::string::npos) {
    s.clear();
    return;
  }

  // Trim right
  size_t last = s.find_last_not_of(" \t\n\r");
  s = s.substr(first, (last - first + 1));
}

std::map<std::string, std::map<std::string, std::string>> IniParser::parse(Stream& stream) {
  std::map<std::string, std::map<std::string, std::string>> sections;
  std::string currentSection;
  String line;

  while (stream.available()) {
    line = stream.readStringUntil('\n');
    std::string sLine = line.c_str();
    trim(sLine);

    if (sLine.empty() || sLine[0] == ';' || sLine[0] == '#') {
      continue;
    }

    if (sLine.front() == '[' && sLine.back() == ']') {
      currentSection = sLine.substr(1, sLine.size() - 2);
      trim(currentSection);
    } else {
      size_t eqPos = sLine.find('=');
      if (eqPos != std::string::npos) {
        std::string key = sLine.substr(0, eqPos);
        std::string value = sLine.substr(eqPos + 1);
        trim(key);
        trim(value);

        // Remove quotes if present
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }

        if (!currentSection.empty()) {
          sections[currentSection][key] = value;
        }
      }
    }
  }
  return sections;
}

std::map<std::string, std::map<std::string, std::string>> IniParser::parseString(const std::string& content) {
  std::map<std::string, std::map<std::string, std::string>> sections;
  std::stringstream ss(content);
  std::string line;
  std::string currentSection;

  while (std::getline(ss, line)) {
    trim(line);

    if (line.empty() || line[0] == ';' || line[0] == '#') {
      continue;
    }

    if (line.front() == '[' && line.back() == ']') {
      currentSection = line.substr(1, line.size() - 2);
      trim(currentSection);
    } else {
      size_t eqPos = line.find('=');
      if (eqPos != std::string::npos) {
        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);
        trim(key);
        trim(value);

        // Remove quotes if present
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
          value = value.substr(1, value.size() - 2);
        }

        if (!currentSection.empty()) {
          sections[currentSection][key] = value;
        }
      }
    }
  }
  return sections;
}

}  // namespace ThemeEngine
