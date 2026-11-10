#pragma once

#include <cstddef>
#include <string_view>

inline constexpr size_t LANGUAGE_CODE_BUFFER_SIZE = 9;

// Lowercase the primary subtag and map ISO 639-2 codes to ISO 639-1.
// Reject missing, non-letter, overlong and placeholder codes. Return later subtags in rest.
[[nodiscard]] bool normalisePrimaryLanguageTag(std::string_view text, char (&out)[LANGUAGE_CODE_BUFFER_SIZE],
                                               std::string_view& rest);
