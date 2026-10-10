#pragma once

#include <cstddef>
#include <string_view>

// Eight primary-subtag letters plus the terminating NUL.
constexpr size_t PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE = 9;

// Trim whitespace, lowercase the subtag before the first '-' or '_', and map
// ISO 639-2 codes to ISO 639-1 ("eng" -> "en"). Requires
// PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE bytes, including the terminating NUL.
// If supplied, rest is a view into tag after that separator; it is empty on failure.
// Returns false for a primary code outside 1..8 ASCII letters, und/mul/zxx/mis,
// or an invalid buffer. Clears out on failure when the buffer has room for a NUL.
[[nodiscard]] bool normalisePrimaryLanguageSubtag(std::string_view tag, char* out, size_t outSize,
                                                  std::string_view* rest = nullptr);
