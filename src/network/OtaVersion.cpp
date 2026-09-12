#include "OtaVersion.h"

#include <algorithm>

namespace ota_version {
namespace {

struct Version {
  std::string_view core[3];
  std::string_view prerelease;
};

bool isDigit(const char value) { return value >= '0' && value <= '9'; }

bool isNumeric(const std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), isDigit);
}

bool validIdentifiers(std::string_view value, const bool prerelease) {
  while (true) {
    const size_t end = value.find('.');
    const auto part = value.substr(0, end);
    if (part.empty()) return false;
    if (!std::all_of(part.begin(), part.end(), [](const char c) {
          return isDigit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
        })) {
      return false;
    }
    if (prerelease && part.size() > 1 && part.front() == '0' && isNumeric(part)) return false;
    if (end == std::string_view::npos) return true;
    value.remove_prefix(end + 1);
  }
}

bool parse(std::string_view input, Version& version, const bool allowDevelopment) {
  if (!input.empty() && input.front() == 'v') input.remove_prefix(1);
  size_t cursor = 0;
  for (size_t part = 0; part < 3; ++part) {
    const size_t start = cursor;
    while (cursor < input.size() && isDigit(input[cursor])) ++cursor;
    if (cursor == start || (cursor - start > 1 && input[start] == '0')) return false;
    version.core[part] = input.substr(start, cursor - start);
    if (part < 2 && (cursor == input.size() || input[cursor++] != '.')) return false;
  }

  // Git branch names may contain '/' and be longer than release tags. Only
  // the local build's generated suffix gets this exception, never a remote tag.
  const auto suffix = input.substr(cursor);
  if (allowDevelopment && suffix.starts_with("-dev-") && suffix.size() > 5) {
    if (!std::all_of(suffix.begin() + 5, suffix.end(),
                     [](const unsigned char c) { return c > 0x20 && c != 0x7F && c != '"' && c != '\\'; })) {
      return false;
    }
    version.prerelease = "dev";
    return true;
  }

  if (cursor < input.size() && input[cursor] == '-') {
    const size_t start = ++cursor;
    while (cursor < input.size() && input[cursor] != '+') ++cursor;
    version.prerelease = input.substr(start, cursor - start);
    if (!validIdentifiers(version.prerelease, true)) return false;
  }
  if (cursor < input.size() && input[cursor] == '+') {
    return validIdentifiers(input.substr(cursor + 1), false);
  }
  return cursor == input.size();
}

int compareNumeric(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
  return left.compare(right);
}

int comparePrerelease(std::string_view left, std::string_view right) {
  while (!left.empty() && !right.empty()) {
    const size_t leftEnd = left.find('.');
    const size_t rightEnd = right.find('.');
    const auto leftPart = left.substr(0, leftEnd);
    const auto rightPart = right.substr(0, rightEnd);
    const bool leftNumeric = isNumeric(leftPart);
    const bool rightNumeric = isNumeric(rightPart);
    const int comparison = leftNumeric && rightNumeric   ? compareNumeric(leftPart, rightPart)
                           : leftNumeric != rightNumeric ? (leftNumeric ? -1 : 1)
                                                         : leftPart.compare(rightPart);
    if (comparison != 0) return comparison;
    left = leftEnd == std::string_view::npos ? std::string_view{} : left.substr(leftEnd + 1);
    right = rightEnd == std::string_view::npos ? std::string_view{} : right.substr(rightEnd + 1);
  }
  return left.empty() == right.empty() ? 0 : left.empty() ? -1 : 1;
}

}  // namespace

bool isNewer(const std::string_view latest, const std::string_view current) {
  Version latestVersion{};
  Version currentVersion{};
  if (!parse(latest, latestVersion, false) || !parse(current, currentVersion, true)) return false;
  for (size_t part = 0; part < 3; ++part) {
    const int comparison = compareNumeric(latestVersion.core[part], currentVersion.core[part]);
    if (comparison != 0) return comparison > 0;
  }
  if (latestVersion.prerelease.empty() || currentVersion.prerelease.empty()) {
    return latestVersion.prerelease.empty() && !currentVersion.prerelease.empty();
  }
  return comparePrerelease(latestVersion.prerelease, currentVersion.prerelease) > 0;
}

}  // namespace ota_version
