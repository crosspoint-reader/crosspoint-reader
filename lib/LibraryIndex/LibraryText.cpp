#include "LibraryText.h"

#include <Utf8.h>
#include <Utf8ComposeTable.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace library {
namespace {

// Letters with no canonical decomposition, plus the punctuation that would
// otherwise become a space and break word shapes. Everything here is a distinct
// letter in Unicode, so no amount of NFD gets us the ASCII form.
struct CharMap {
  uint32_t cp;
  const char* replacement;
};
constexpr CharMap EXPLICIT_MAP[] = {
    {0x00D8, "o"},   // Ø
    {0x00F8, "o"},   // ø  — "Søren" folds to "soren", not "nesb"
    {0x00C6, "ae"},  // Æ
    {0x00E6, "ae"},  // æ
    {0x0152, "oe"},  // Œ
    {0x0153, "oe"},  // œ
    {0x00DF, "ss"},  // ß
    {0x0141, "l"},   // Ł
    {0x0142, "l"},   // ł
    {0x00D0, "d"},   // Ð
    {0x00F0, "d"},   // ð
    {0x0110, "d"},   // Đ
    {0x0111, "d"},   // đ
    {0x00DE, "th"},  // Þ
    {0x00FE, "th"},  // þ
    {0x0131, "i"},   // ı
    {0x0027, "'"},   // ' — kept, not turned into a space, so "O'Brien" stays one word
    {0x2019, "'"},   // ’ — and so the curly form folds to the same thing
    {0x2018, "'"},   // ‘
};

const char* explicitMapping(const uint32_t cp) {
  for (const auto& e : EXPLICIT_MAP) {
    if (e.cp == cp) return e.replacement;
  }
  return nullptr;
}

// Reverse lookup in the NFC table: given a precomposed codepoint, return the
// base letter it was composed from, or 0.
//
// The table is sorted by (base, mark) so this is a linear scan rather than a
// binary search. It runs once per non-ASCII character at index-build time and
// over a short query per keystroke, never over the whole index, so the scan is
// not on any hot path. Reusing the generated table costs no extra flash — it is
// already linked for utf8ComposeNfc().
uint32_t decomposedBase(const uint32_t cp) {
  if (cp < 0x00C0) return 0;  // no precomposed Latin letter below this
  for (const auto& e : kUtf8ComposeTable) {
    if (e.composed == cp) return e.base;
  }
  return 0;
}

// Fully decompose: é -> e, and the rare doubly-accented forms (ế -> ê -> e).
uint32_t stripDiacritics(uint32_t cp) {
  for (int guard = 0; guard < 4; guard++) {
    const uint32_t base = decomposedBase(cp);
    if (base == 0) break;
    cp = base;
  }
  return cp;
}

bool isAsciiAlnum(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

void appendLowerAscii(const uint32_t cp, std::string& out) {
  out.push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp));
}

// Articles stripped from the head of sort and search keys. Display text never
// goes through this.
constexpr const char* ARTICLES[] = {"the ", "a ",   "an ", "le ",  "la ",  "les ", "l'",   "un ",
                                    "une ", "de ",  "du ", "des ", "der ", "die ", "das ", "el ",
                                    "los ", "las ", "il ", "lo ",  "gli ", "i ",   "o ",   "os "};

// Words that must not be the last word of a filename title for TITLE_MERGE to
// fire: an exporter truncating mid-phrase almost always stops on one of these.
constexpr const char* STOP_TAIL[] = {"a",    "an",   "the", "of",   "to",   "for",  "at", "if",  "in",
                                     "on",   "and",  "or",  "with", "from", "your", "is", "as",  "by",
                                     "de",   "du",   "des", "la",   "le",   "les",  "un", "une", "et",
                                     "pour", "dans", "sur", "que",  "qui",  "ce",   "d",  "l",   "e"};

bool isStopTail(const std::string& word) {
  for (const char* s : STOP_TAIL) {
    if (word == s) return true;
  }
  return false;
}

void splitTokens(std::string_view folded, std::string* out, size_t maxTokens, size_t& count) {
  count = 0;
  size_t i = 0;
  while (i < folded.size() && count < maxTokens) {
    while (i < folded.size() && folded[i] == ' ') i++;
    const size_t start = i;
    while (i < folded.size() && folded[i] != ' ') i++;
    if (i > start) out[count++].assign(folded.substr(start, i - start));
  }
}

bool looksLikeHexDigest(std::string_view s) {
  if (s.empty()) return false;
  // At least one decimal digit is required: "Bede" and "Abba" are authors,
  // not digests, and a real hash without a single digit is vanishingly rare.
  // The trade-off runs the other way for pure-letter strings like "deadbeef",
  // which now read as an (odd) author instead of being dropped — harmless.
  bool sawDigit = false;
  for (const char c : s) {
    const bool digit = (c >= '0' && c <= '9');
    if (!digit && (c < 'a' || c > 'f')) return false;
    sawDigit = sawDigit || digit;
  }
  return sawDigit;
}

bool allDigits(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// A plausible publication year, 1500-2099. Narrow on purpose: a wider range
// would swallow titles like "1987" and "2085".
bool isYear(std::string_view s) {
  if (s.size() != 4 || !allDigits(s)) return false;
  const int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  return (y >= 1500 && y <= 2099);
}

// Drop a trailing run of spaces and hyphens. Exporters leave "Paris, 1985 -"
// when a later field was empty.
std::string_view trimTrailingDashes(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '-')) s.remove_suffix(1);
  return s;
}

}  // namespace

std::string fold(const std::string_view text, const bool stripArticle) {
  std::string out;
  out.reserve(text.size());

  const auto* cursor = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = cursor + text.size();
  bool pendingSpace = false;

  while (cursor < end) {
    // The shared decoder stops on NUL and every std::string source is
    // NUL-terminated, but fold's contract is string_view — and a view may end
    // mid-sequence. Refuse to decode a lead byte whose continuation bytes lie
    // past the end rather than trusting whatever sits there.
    const unsigned char lead = *cursor;
    const ptrdiff_t promised = lead < 0x80           ? 1
                               : (lead >> 5) == 0x06 ? 2
                               : (lead >> 4) == 0x0E ? 3
                               : (lead >> 3) == 0x1E ? 4
                                                     : 1;
    if (promised > end - cursor) break;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;

    if (utf8IsCombiningMark(cp)) continue;  // already-decomposed input

    const char* mapped = explicitMapping(cp);
    if (mapped != nullptr) {
      if (pendingSpace && !out.empty()) out.push_back(' ');
      pendingSpace = false;
      out.append(mapped);
      continue;
    }

    const uint32_t base = stripDiacritics(cp);
    if (isAsciiAlnum(base)) {
      if (pendingSpace && !out.empty()) out.push_back(' ');
      pendingSpace = false;
      appendLowerAscii(base, out);
      continue;
    }

    // Everything else — punctuation, symbols, unmapped scripts — separates
    // words. Deferring the space keeps runs collapsed and drops trailing ones.
    if (!out.empty()) pendingSpace = true;
  }

  if (stripArticle) {
    for (const char* article : ARTICLES) {
      const size_t len = strlen(article);
      if (out.size() > len && out.compare(0, len, article) == 0) {
        out.erase(0, len);
        break;
      }
    }
  }
  return out;
}

bool looksLikeMetadata(const std::string_view raw) {
  // Takes the RAW segment, not a folded one. The fold turns ',' ';' and '#' into
  // spaces, and the "<publisher>, <year>" shape is defined by exactly those
  // separators — folding first would make that rule unable to fire at all.
  const std::string_view trimmed = trimTrailingDashes(raw);
  if (trimmed.empty()) return true;

  const std::string folded = fold(trimmed);
  if (folded.empty()) return true;

  if (folded.size() >= 4 && folded.size() <= 32 && looksLikeHexDigest(folded)) return true;
  if (folded.rfind("isbn10", 0) == 0 || folded.rfind("isbn13", 0) == 0) return true;
  // Organisation credits that export tools leave in the author slot:
  // "Internet Archive", "Open Library" and the like are sources, not people.
  // Matching the last word keeps this free of any hardcoded site list, and no
  // person's surname is "Archive" or "Library".
  const size_t lastSpace = folded.find_last_of(' ');
  const std::string_view lastWord =
      lastSpace == std::string::npos ? std::string_view(folded) : std::string_view(folded).substr(lastSpace + 1);
  if (lastWord == "archive" || lastWord == "library") return true;
  if (folded.size() == 13 && allDigits(folded) && folded.rfind("97", 0) == 0) return true;
  if (folded.size() == 10 && allDigits(folded.substr(0, 9)) &&
      (folded[9] == 'x' || (folded[9] >= '0' && folded[9] <= '9'))) {
    return true;
  }
  if (isYear(folded)) return true;

  // "<place or publisher>, 2019" and "<series>; DL 2009" — a short-ish head, a
  // separator, then a year. The head bound keeps this from firing on a long
  // title that happens to end in a number. Punctuation comes from the raw text;
  // the tail is folded so "DL" and "dl" behave alike.
  const size_t sep = trimmed.find_last_of(",;#");
  if (sep != std::string_view::npos && sep <= 80) {
    std::string tail = fold(trimmed.substr(sep + 1));
    if (tail.rfind("dl ", 0) == 0) tail.erase(0, 3);
    if (isYear(tail)) return true;
  }
  return false;
}

ParsedName parseFilename(const std::string_view stem) {
  ParsedName out;
  constexpr std::string_view SEP = " -- ";

  const size_t first = stem.find(SEP);
  if (first == std::string_view::npos) {
    out.title.assign(stem);
    return out;
  }

  out.title.assign(stem.substr(0, first));

  const size_t secondStart = first + SEP.size();
  const size_t second = stem.find(SEP, secondStart);
  const std::string_view candidate =
      stem.substr(secondStart, second == std::string_view::npos ? std::string_view::npos : second - secondStart);

  // Segments 2..n are never read. Only this one candidate is classified.
  if (!looksLikeMetadata(candidate)) out.author.assign(candidate);

  if (out.title.empty()) out.title.assign(stem);
  return out;
}

std::string cleanPersonName(const std::string_view author) {
  std::string out;
  out.reserve(author.size());
  int depth = 0;
  for (size_t i = 0; i < author.size(); i++) {
    const char c = author[i];
    if (c == '[' || c == '(') {
      depth++;
      continue;
    }
    if (c == ']' || c == ')') {
      if (depth > 0) depth--;
      continue;
    }
    if (c == ';') break;  // secondary authors
    if (depth > 0) continue;
    if (c == '_') {
      // "Herbert G_ Wells" — the underscore stands in for a full stop the
      // filesystem would not take. Between letters it is an abbreviation dot;
      // at the end of a word it is just noise.
      const bool betweenLetters = i > 0 && i + 1 < author.size() &&
                                  isalpha(static_cast<unsigned char>(author[i - 1])) &&
                                  isalpha(static_cast<unsigned char>(author[i + 1]));
      out.push_back(betweenLetters ? '.' : ' ');
      continue;
    }
    out.push_back(c);
  }

  while (!out.empty() &&
         (out.back() == ' ' || out.back() == ',' || out.back() == '-' || out.back() == '.' || out.back() == '_')) {
    out.pop_back();
  }
  size_t start = 0;
  while (start < out.size() && out[start] == ' ') start++;
  out.erase(0, start);

  // Collapse the space runs left behind by the removals.
  std::string collapsed;
  collapsed.reserve(out.size());
  bool space = false;
  for (const char c : out) {
    if (c == ' ') {
      space = true;
      continue;
    }
    if (space && !collapsed.empty()) collapsed.push_back(' ');
    space = false;
    collapsed.push_back(c);
  }

  // "Austen, Jane" is the same person as "Jane Austen", and publishers use both.
  // The spelling vote cannot settle it — with one book per author there is no
  // majority — so the inverted form is turned round here instead. Only a single
  // comma qualifies: "Smith, John, Jr." and lists of several authors are left
  // exactly as they are rather than being scrambled.
  const size_t comma = collapsed.find(',');
  if (comma != std::string::npos && collapsed.find(',', comma + 1) == std::string::npos) {
    std::string_view last(collapsed.data(), comma);
    std::string_view first(collapsed.data() + comma + 1, collapsed.size() - comma - 1);
    while (!first.empty() && first.front() == ' ') first.remove_prefix(1);
    while (!last.empty() && last.back() == ' ') last.remove_suffix(1);
    if (!first.empty() && !last.empty()) {
      std::string swapped;
      swapped.reserve(collapsed.size());
      swapped.append(first);
      swapped.push_back(' ');
      swapped.append(last);
      return swapped;
    }
  }
  return collapsed;
}

std::string authorKey(const std::string_view author) {
  // Drop bracketed spans ("George Sand [Sand, George]") and everything after
  // a multi-author separator.
  std::string cleaned;
  cleaned.reserve(author.size());
  int depth = 0;
  for (const char c : author) {
    if (c == '[' || c == '(') {
      depth++;
      continue;
    }
    if (c == ']' || c == ')') {
      if (depth > 0) depth--;
      continue;
    }
    if (c == ';') break;
    if (depth == 0) cleaned.push_back(c);
  }

  const std::string folded = fold(cleaned);

  constexpr size_t MAX_TOKENS = 12;
  std::string tokens[MAX_TOKENS];
  size_t count = 0;
  splitTokens(folded, tokens, MAX_TOKENS, count);

  // Initials carry no identity and appear inconsistently ("Herbert G Wells" vs
  // "Herbert Wells"), so they must not change the key.
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    if (tokens[i].size() > 1) tokens[kept++] = tokens[i];
  }
  std::sort(tokens, tokens + kept);

  std::string key;
  for (size_t i = 0; i < kept; i++) {
    if (!key.empty()) key.push_back(' ');
    key += tokens[i];
  }
  // Truncate on bytes, not on a token boundary. Sorting puts a short forename
  // first, so a whole-token cut would reduce "Wollstonecraft, Mary" to the key
  // "alex" and merge every Alex in the library; the byte cut keeps
  // "mary wollsto", which stays a prefix of the full key and discriminates.
  if (key.size() > AUTHOR_KEY_MAX_BYTES) key.resize(AUTHOR_KEY_MAX_BYTES);
  while (!key.empty() && key.back() == ' ') key.pop_back();
  return key;
}

bool preferFilenameTitle(const std::string_view dcTitle, const std::string_view fnTitle) {
  const std::string dc = fold(dcTitle);
  const std::string fn = fold(fnTitle);
  if (dc.empty() || fn.empty()) return false;
  if (fn.size() <= dc.size()) return false;
  if (fn.compare(0, dc.size(), dc) != 0) return false;
  if (fn[dc.size()] != ' ') return false;  // word boundary, not a mid-word prefix

  constexpr size_t MAX_TOKENS = 24;
  std::string extra[MAX_TOKENS];
  size_t count = 0;
  splitTokens(std::string_view(fn).substr(dc.size() + 1), extra, MAX_TOKENS, count);
  if (count == 0) return false;

  size_t substantial = 0;
  for (size_t i = 0; i < count; i++) {
    if (extra[i].size() >= 3) substantial++;
  }
  if (substantial < 2) return false;

  return !isStopTail(extra[count - 1]);
}

// fold() keeps the apostrophe, which is right for sorting — "L'Eneide" belongs
// under L. For searching it is wrong: in French the word worth typing is the one
// AFTER the apostrophe, so "eneide" must reach "L'Eneide" and "cote" must
// reach "d'a cote". Treating it as a word boundary here leaves the sort untouched.
bool isWordBreak(const char c) { return c == ' ' || c == '\''; }

bool matchesQuery(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) return true;

  // Walk the query one word at a time, and for each one scan the book's words for
  // a prefix hit. Both strings are at most a couple of hundred bytes and this
  // runs once per book per keypress, so a plain scan is cheaper than anything
  // that would need building first.
  size_t qs = 0;
  while (qs < needle.size()) {
    while (qs < needle.size() && isWordBreak(needle[qs])) qs++;
    if (qs >= needle.size()) break;
    size_t qe = qs;
    while (qe < needle.size() && !isWordBreak(needle[qe])) qe++;
    const std::string_view word = needle.substr(qs, qe - qs);

    bool found = false;
    size_t hs = 0;
    while (hs < haystack.size() && !found) {
      while (hs < haystack.size() && isWordBreak(haystack[hs])) hs++;
      if (hs >= haystack.size()) break;
      if (haystack.compare(hs, word.size(), word) == 0) {
        found = true;
        break;
      }
      while (hs < haystack.size() && !isWordBreak(haystack[hs])) hs++;
    }
    if (!found) return false;
    qs = qe;
  }
  return true;
}

std::string surnameKey(const std::string_view displayAuthor) {
  const std::string folded = fold(displayAuthor);
  if (folded.empty()) return {};

  size_t lastStart = std::string::npos;
  size_t lastEnd = folded.size();
  size_t i = folded.size();
  while (i > 0) {
    i--;
    if (folded[i] != ' ') {
      if (lastStart == std::string::npos) lastEnd = i + 1;
      lastStart = i;
    } else if (lastStart != std::string::npos) {
      break;
    }
  }
  if (lastStart == std::string::npos) return {};

  std::string key;
  key.reserve(folded.size() + 1);
  key.append(folded, lastStart, lastEnd - lastStart);
  // The given names follow, so two people sharing a surname stay in a stable,
  // readable order rather than whichever the disk walk happened to produce.
  if (lastStart > 0) {
    key.push_back(' ');
    key.append(folded, 0, lastStart);
    while (!key.empty() && key.back() == ' ') key.pop_back();
  }
  return key;
}

}  // namespace library
