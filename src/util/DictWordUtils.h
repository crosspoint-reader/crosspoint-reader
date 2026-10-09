#pragma once

#include <string>

// Pure text helpers for dictionary lookups (no SD or Arduino dependencies, so
// they are host-testable).
namespace DictWordUtils {

// The word with surrounding punctuation stripped (ASCII non-alphanumerics and
// General Punctuation U+2000-U+206F such as curly quotes and dashes). Case is
// preserved; empty when nothing word-like remains.
std::string trimWordEdges(const char* word);

// Display priority of a matched headword for the word as it appears in the
// text (case preserved, edges trimmed); lower sorts first. An exact-case match
// wins, then headwords that don't start with an ASCII capital, so lowercase
// "laconic" shows the adjective before the proper noun "Laconic".
int headwordRank(const char* headword, const char* textWord);

}  // namespace DictWordUtils
