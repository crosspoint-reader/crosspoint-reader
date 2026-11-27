#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Words found in the dictionary, from a book or from Home search, newest
// first and without repeats, in /.crosspoint/dictionary_history.txt (one word
// per line).
namespace dicthistory {

inline constexpr size_t MAX_WORDS = 200;

// Fills out with up to MAX_WORDS words, newest first; a missing file gives none.
void load(std::vector<std::string>& out);

// Moves word to the front (case-insensitive match), dropping the oldest past MAX_WORDS.
void add(const std::string& word);

}  // namespace dicthistory
