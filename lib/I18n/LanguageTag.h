#pragma once

#include <cstddef>
#include <string_view>

// Minimum output buffer size, including the terminating NUL.
constexpr size_t LANGUAGE_TAG_BUFFER_SIZE = 16;

// Normalise the primary language code ("eng" -> "en"). Keep the second subtag
// only if it matches a translation ("pt-br" -> "pt-BR"); discard later subtags.
// Requires LANGUAGE_TAG_BUFFER_SIZE bytes, including the terminating NUL.
// Returns false for an invalid or placeholder primary code or an invalid buffer.
// Clears out on failure when the buffer has room for a NUL.
[[nodiscard]] bool normaliseLanguageTag(std::string_view tag, char* out, size_t outSize);

// Return the native language name for an exact, case-sensitive translation tag
// ("fr" -> "Français"), or nullptr if no translation matches.
[[nodiscard]] const char* languageNameForTag(const char* tag);
