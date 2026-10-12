#pragma once

#include <optional>
#include <string>

struct PackageGroupFields {
  std::string series;                // The series name
  std::optional<float> seriesIndex;  // Position within the series (e.g. 3.5)
  std::string publisher;             // The first non-blank publisher
  std::string language;              // The first non-blank language
  std::string subject;               // The first non-blank subject
};
