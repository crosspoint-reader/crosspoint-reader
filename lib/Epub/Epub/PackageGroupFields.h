#pragma once

#include <string>

// Library grouping metadata. Publisher, language and subject use the first non-blank value.
struct PackageGroupFields {
  std::string series;
  std::string seriesIndexText;  // Original position text, such as "3.5".
  std::string publisher;
  std::string language;
  std::string subject;
};
