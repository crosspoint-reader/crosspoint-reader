#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace DictionaryQuery {

constexpr size_t MAX_PHRASE_TOKENS = 4;
constexpr size_t MAX_QUERY_BYTES = 255;

std::string clean(std::string_view text);
bool buildPhrase(const char* const* words, size_t count, std::string& out);

}  // namespace DictionaryQuery
