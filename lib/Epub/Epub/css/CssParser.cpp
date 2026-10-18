#include "CssParser.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <new>
#include <string_view>

// Sparse resident-style codec (defined below, used by lookupRules before its definition).
static size_t compressStyle(const CssStyle& s, uint8_t* out);
static void decompressStyle(const uint8_t* in, CssStyle& out);

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;
  bool overflowed = false;  // set once content exceeds CAPACITY so callers can reject the whole token

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    } else {
      overflowed = true;  // dropping this char would silently truncate — flag it instead
    }
  }

  void clear() {
    len = 0;
    overflowed = false;
  }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }

  // Heap copy of the token. Kept for callers that genuinely need to own the bytes; the parse
  // loop uses view() instead — its consumers take string_view, so str()'s allocation there was
  // pure waste (one per declaration plus one per rule block, i.e. thousands of short-lived
  // blocks per stylesheet, each leaving a hole the heap never reclaims).
  std::string str() const { return std::string(data, len); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
#ifndef CSS_MIN_FREE_HEAP_FOR_CSS
#define CSS_MIN_FREE_HEAP_FOR_CSS (40 * 1024)
#endif

constexpr size_t MIN_FREE_HEAP_FOR_CSS = CSS_MIN_FREE_HEAP_FOR_CSS;

// Lean-mode floor (setLeanResolve): the index lives off-heap in the arena and the hot cache
// is disabled, so resolveStyle allocates only a few small transient strings. The real fault
// zone is ~13-15 KB free (see EpubReaderActivity RESIDENT_BUILD_ABORT_*), so 24 KB keeps a
// comfortable margin while letting a borrowed build (~40 KB free) resolve without degrading.
#ifndef CSS_LEAN_MIN_FREE_HEAP_FOR_CSS
#define CSS_LEAN_MIN_FREE_HEAP_FOR_CSS (24 * 1024)
#endif

constexpr size_t LEAN_MIN_FREE_HEAP_FOR_CSS = CSS_LEAN_MIN_FREE_HEAP_FOR_CSS;

// In-memory CSS rule cache sizing for disk-backed lookup mode.
// Keeps memory bounded on large books while retaining hot selectors.
#ifndef CSS_HOT_RULE_CACHE_SIZE
#define CSS_HOT_RULE_CACHE_SIZE 128
#endif

#ifndef CSS_NEGATIVE_CACHE_SIZE
#define CSS_NEGATIVE_CACHE_SIZE 256
#endif

constexpr size_t HOT_RULE_CACHE_SIZE = CSS_HOT_RULE_CACHE_SIZE;
constexpr size_t NEGATIVE_CACHE_SIZE = CSS_NEGATIVE_CACHE_SIZE;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
// Layout: 4 enum bytes + 11 lengths + display byte + definedBits uint16 + 2 vertAlign bytes + cssFloat byte
//         + smallCaps byte + fontSizeMultiplier float + fontSize flags byte + block flags byte
//         (listStyleNone / pageBreakBefore / pageBreakAfter value+defined pairs)
//         + visibility flags byte (colorTransparent / opacityZero / visibilityHidden value+defined pairs; v19)
//         + fontFamily hash (uint32; defined is bit 2 of the font-size flags byte; v28)
constexpr size_t CSS_FIXED_STYLE_BYTES = 4 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) +
                                         sizeof(uint8_t) + sizeof(uint16_t) + 2 * sizeof(uint8_t) + sizeof(uint8_t) +
                                         sizeof(uint8_t) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t) +
                                         sizeof(uint8_t) + sizeof(uint32_t);
static_assert(CSS_FIXED_STYLE_BYTES == 77,
              "style payload layout changed — update read/writeCssStylePayload and bump CSS_CACHE_VERSION");
// A rule record is selectorLen(2) selector order(2) payload; the order sits between selector and
// payload so a reader that has matched the selector reads the rest in one go (v22).
constexpr size_t CSS_RULE_ORDER_BYTES = sizeof(uint16_t);

// Specificity, packed so that comparing two values compares (ids, classes, element names) in that
// order: five bits each, the element count lowest. resolveStyle ranks a rule as
// specificity << 16 | source order (CssStyle::applyOver), so a rank is ordered by specificity first.
constexpr uint32_t SPECIFICITY_TAG = 1u;
constexpr uint32_t SPECIFICITY_CLASS = 1u << 5;
constexpr uint32_t SPECIFICITY_ID = 1u << 10;

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";
// Background pictures (CssParser::backgroundImageFor), when the book has any:
// version(1) + count(2) + count x {selectorLen(2) selector pathLen(2) path}.
constexpr char backgroundImagesFile[] = "/css_bg.bin";
constexpr uint8_t BACKGROUND_IMAGES_FILE_VERSION = 1;
// @font-face rules (CssParser::loadFontFaces), when the book has any: CSS_CACHE_VERSION(1), then
// one record per face to the end of the file: {weight(2) italic(1) nameLen(1) name hrefLen(1) href}.
constexpr char fontFacesFile[] = "/css_fonts.bin";
constexpr size_t MAX_FONT_FACE_TEXT = 255;  // a name or href longer than this is not recorded
constexpr char compileTempRulesCache[] = "/css_rules.compile.tmp";

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Resolver supports: tag, .class, tag.class, #id, tag#id
bool isSelectorUsableByResolver(std::string_view selector) {
  if (selector.empty()) {
    return false;
  }

  // Reject combinators, pseudo-classes, wildcards, and whitespace
  if (selector.find_first_of("+>:[~* ") != std::string_view::npos) {
    return false;
  }

  const size_t hashPos = selector.find('#');
  const size_t dotPos = selector.find('.');

  // Reject selectors with both # and . (too complex)
  if (hashPos != std::string_view::npos && dotPos != std::string_view::npos) {
    return false;
  }

  if (hashPos != std::string_view::npos) {
    // #id  (hashPos == 0, must have content after)
    if (hashPos == 0) {
      return selector.size() > 1 && selector.find('#', 1) == std::string_view::npos;
    }
    // tag#id (no second #, must have content after #)
    return hashPos + 1 < selector.size() && selector.find('#', hashPos + 1) == std::string_view::npos;
  }

  if (dotPos == std::string_view::npos) {
    return true;  // tag
  }

  if (dotPos == 0) {
    return selector.size() > 1 && selector.find('.', 1) == std::string_view::npos;  // .class only
  }

  // tag.class only, no additional dots
  return dotPos + 1 < selector.size() && selector.find('.', dotPos + 1) == std::string_view::npos;
}

// FNV-1a 32-bit (CssParser::selectorHash, and the names of complex-rule compounds).
uint32_t fnv1a32(const std::string_view s) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// The complex-rule table (CssParser::complexTable). Caps keep it a few KB at most; a selector
// past one is counted unsupported like any other the resolver cannot use.
constexpr size_t MAX_COMPLEX_BYTES = 4096;
constexpr uint8_t MAX_COMPLEX_COMPOUNDS = 64;  // one bit each in an element's u64 compound mask
constexpr uint8_t MAX_COMPLEX_RULES = 255;
constexpr size_t MAX_COMPOUND_CLASSES = 7;  // the low 3 bits of a compound's flags byte
constexpr size_t COMPLEX_HEADER_BYTES = 4;  // compoundCount(1) ruleCount(1) compoundBytes(2)
constexpr size_t COMPLEX_RULE_HEADER_BYTES = 6;
constexpr uint8_t COMPOUND_HAS_TAG = 0x80;
constexpr uint8_t COMPOUND_HAS_ID = 0x40;
constexpr uint8_t COMPOUND_CLASS_COUNT = 0x07;
// An element's classes past this many take no part in compound matching.
constexpr size_t MAX_ELEMENT_CLASSES = 8;
// A complex rule's steps: the compound index, and whether the step must be the parent of the one
// before it (child combinator) rather than any ancestor (descendant combinator).
constexpr uint8_t STEP_COMPOUND = 0x3F;
constexpr uint8_t STEP_CHILD = 0x40;
constexpr size_t MAX_COMPLEX_STEPS = 8;

bool isNameChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

// One compound selector -- an element name or `*`, then any .class and at most one #id -- with
// its names hashed. False for anything else: an escape, an attribute or pseudo part, a combinator.
struct Compound {
  uint8_t flags = 0;
  uint32_t tag = 0;
  uint32_t id = 0;
  uint32_t classes[MAX_COMPOUND_CLASSES] = {};

  uint8_t classCount() const { return flags & COMPOUND_CLASS_COUNT; }
  uint32_t specificity() const {
    return ((flags & COMPOUND_HAS_ID) ? SPECIFICITY_ID : 0) + classCount() * SPECIFICITY_CLASS +
           ((flags & COMPOUND_HAS_TAG) ? SPECIFICITY_TAG : 0);
  }
  // The table form: flags, then the names it has, classes sorted.
  size_t serialize(uint8_t* out) const {
    uint8_t* p = out;
    *p++ = flags;
    if (flags & COMPOUND_HAS_TAG) p = static_cast<uint8_t*>(std::memcpy(p, &tag, 4)) + 4;
    if (flags & COMPOUND_HAS_ID) p = static_cast<uint8_t*>(std::memcpy(p, &id, 4)) + 4;
    for (uint8_t i = 0; i < classCount(); ++i) p = static_cast<uint8_t*>(std::memcpy(p, &classes[i], 4)) + 4;
    return static_cast<size_t>(p - out);
  }
};
constexpr size_t MAX_COMPOUND_BYTES = 1 + 4 + 4 + 4 * MAX_COMPOUND_CLASSES;

bool parseCompound(const std::string_view text, Compound& out) {
  out = Compound{};
  if (text.empty()) return false;
  size_t i = 0;
  if (text[0] == '*') {
    i = 1;
  } else {
    while (i < text.size() && isNameChar(text[i])) ++i;
    if (i > 0) {
      out.flags |= COMPOUND_HAS_TAG;
      out.tag = fnv1a32(text.substr(0, i));
    }
  }
  uint8_t classCount = 0;
  while (i < text.size()) {
    const char marker = text[i++];
    const size_t start = i;
    while (i < text.size() && isNameChar(text[i])) ++i;
    if (i == start) return false;
    const uint32_t name = fnv1a32(text.substr(start, i - start));
    if (marker == '#' && !(out.flags & COMPOUND_HAS_ID)) {
      out.flags |= COMPOUND_HAS_ID;
      out.id = name;
    } else if (marker == '.' && classCount < MAX_COMPOUND_CLASSES) {
      out.classes[classCount++] = name;
    } else {
      return false;
    }
  }
  std::sort(out.classes, out.classes + classCount);
  out.flags |= classCount;
  return true;
}

uint16_t readU16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

size_t compoundSize(const uint8_t flags) {
  return 1 + ((flags & COMPOUND_HAS_TAG) ? 4 : 0) + ((flags & COMPOUND_HAS_ID) ? 4 : 0) +
         4 * (flags & COMPOUND_CLASS_COUNT);
}

// Bounds-checks a table read from the cache, so a damaged file cannot send the per-element scan
// past its end.
bool complexTableValid(const uint8_t* table, const size_t bytes) {
  if (bytes < COMPLEX_HEADER_BYTES) return false;
  size_t pos = COMPLEX_HEADER_BYTES;
  for (uint8_t i = 0; i < table[0]; ++i) {
    if (pos >= bytes) return false;
    pos += compoundSize(table[pos]);
  }
  if (pos != COMPLEX_HEADER_BYTES + readU16(table + 2)) return false;
  for (uint8_t r = 0; r < table[1]; ++r) {
    if (pos + COMPLEX_RULE_HEADER_BYTES > bytes) return false;
    const uint8_t steps = table[pos + 4];
    const uint8_t styleLen = table[pos + 5];
    if (steps == 0 || pos + COMPLEX_RULE_HEADER_BYTES + steps + styleLen > bytes) return false;
    for (uint8_t s = 0; s < steps; ++s) {
      if ((table[pos + COMPLEX_RULE_HEADER_BYTES + s] & STEP_COMPOUND) >= table[0]) return false;
    }
    pos += COMPLEX_RULE_HEADER_BYTES + steps + styleLen;
  }
  return pos == bytes;
}

// FNV-1a of the lowercased name, without building the lowercase copy.
uint32_t fnv1a32Lower(const std::string_view s) {
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
    h *= 16777619u;
  }
  return h;
}

// The specificity of a selector is the sum of its compounds', field by field, each field
// saturating at its five bits.
uint32_t addSpecificity(const uint32_t a, const uint32_t b) {
  uint32_t sum = 0;
  for (int shift = 0; shift <= 10; shift += 5) {
    sum |= std::min<uint32_t>(31, ((a >> shift) & 31) + ((b >> shift) & 31)) << shift;
  }
  return sum;
}

// Whether a complex rule's ancestor steps (steps[1..count)) sit on the element's ancestors, whose
// compound masks are masks[0..depth), masks[depth - 1] the parent. Right to left: a run of child
// steps must sit on consecutive ancestors, and a descendant step starts a run that may sit on any
// ancestor above the last one matched. Placing each run as near as it fits leaves the most
// ancestors to the runs further left, so the first fit is the answer and nothing backtracks:
// linear in depth x steps whatever the sheet.
bool chainMatches(const uint8_t* steps, const uint8_t count, const uint64_t* masks, size_t depth) {
  for (uint8_t i = 1; i < count;) {
    uint8_t end = i + 1;
    while (end < count && (steps[end] & STEP_CHILD)) ++end;
    const size_t run = end - i;
    // The run with step i on masks[top], step i + k on masks[top - k].
    const auto runAt = [&](const size_t top) {
      if (top + 1 < run) return false;
      for (size_t k = 0; k < run; ++k) {
        if (!((masks[top - k] >> (steps[i + k] & STEP_COMPOUND)) & 1u)) return false;
      }
      return true;
    };
    if (depth == 0) return false;
    size_t top = depth - 1;
    if (steps[i] & STEP_CHILD) {
      if (!runAt(top)) return false;
    } else {
      while (!runAt(top)) {
        if (top == 0) return false;
        --top;
      }
    }
    depth = top + 1 - run;
    i = end;
  }
  return true;
}

template <typename Fn>
void forEachNormalizedClassToken(const std::string& classAttr, std::string& normalizedBuf, Fn&& fn) {
  size_t i = 0;
  while (i < classAttr.size()) {
    while (i < classAttr.size() && isCssWhitespace(classAttr[i])) {
      ++i;
    }
    if (i >= classAttr.size()) {
      break;
    }

    const size_t start = i;
    while (i < classAttr.size() && !isCssWhitespace(classAttr[i])) {
      ++i;
    }

    normalizedBuf.clear();
    normalizedBuf.reserve(i - start);
    for (size_t j = start; j < i; ++j) {
      normalizedBuf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(classAttr[j]))));
    }
    if (!normalizedBuf.empty()) {
      fn(normalizedBuf);
    }
  }
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (value.substr(suffixPos) != IMPORTANT) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::string_view trimCss(std::string_view v) {
  while (!v.empty() && isCssWhitespace(v.front())) v.remove_prefix(1);
  while (!v.empty() && isCssWhitespace(v.back())) v.remove_suffix(1);
  return v;
}

// `lower` is lowercase: compares `a` to it ignoring ASCII case.
bool equalsIgnoreCase(const std::string_view a, const std::string_view lower) {
  if (a.size() != lower.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != lower[i]) return false;
  }
  return true;
}

// Where lowercase `needle` first occurs in `hay`, ignoring ASCII case.
size_t findIgnoreCase(const std::string_view hay, const std::string_view needle) {
  if (needle.size() > hay.size()) return std::string_view::npos;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && std::tolower(static_cast<unsigned char>(hay[i + k])) == needle[k]) ++k;
    if (k == needle.size()) return i;
  }
  return std::string_view::npos;
}

std::string_view unquoteCss(std::string_view v) {
  v = trimCss(v);
  if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
    v = trimCss(v.substr(1, v.size() - 2));
  }
  return v;
}

// Takes the first `name: value` declaration off `rest` (a block's ';'-separated declarations),
// both trimmed. A declaration without a colon is passed over; false when none is left.
bool nextDeclaration(std::string_view& rest, std::string_view& name, std::string_view& value) {
  while (!rest.empty()) {
    const std::string_view decl(rest.data(), std::min(rest.find(';'), rest.size()));
    rest.remove_prefix(std::min(decl.size() + 1, rest.size()));
    const size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    name = trimCss(std::string_view(decl.data(), colon));
    value = trimCss(std::string_view(decl.data() + colon + 1, decl.size() - colon - 1));
    return true;
  }
  return false;
}

// A font-family list (normalized text) as CssStyle::fontFamily keeps it: the first family hashed, 0 when it is a
// generic family or a keyword, which leave the text in the reader's font; with the list's first generic serif or
// sans-serif in the low bits (CssFontFamily). Ported from crosspoint-reader PR #3934 (Justin Mitchell,
// @itsthisjustin); the generic fallback is ours.
uint32_t firstFontFamily(std::string_view value) {
  constexpr std::string_view GENERIC[] = {"serif",   "sans-serif", "monospace", "cursive",  "fantasy",
                                          "inherit", "initial",    "unset",     "system-ui"};
  constexpr size_t kNotGeneric = sizeof(GENERIC) / sizeof(GENERIC[0]);
  uint32_t identity = 0;
  for (bool first = true;; first = false) {
    const size_t comma = value.find(',');
    const std::string_view name = unquoteCss(value.substr(0, comma));
    size_t generic = 0;
    while (generic < kNotGeneric && name != GENERIC[generic]) ++generic;
    if (generic < 2) return identity | static_cast<uint32_t>(generic + 1);  // CssFontFamily::kSerif, kSansSerif
    if (first && generic == kNotGeneric && !name.empty()) identity = CssParser::fontFamilyHash(name);
    if (generic < kNotGeneric || comma == std::string_view::npos) return identity;
    value.remove_prefix(comma + 1);
  }
}

}  // anonymous namespace

// FNV-1a 32-bit hash — no heap, good distribution over short selector strings.
// 32 bits keep the selector index at 8 bytes/entry; collisions are resolved by
// verifying the on-disk selector string in readRuleFromDiskAtOffset().
uint32_t CssParser::selectorHash(std::string_view s) { return fnv1a32(s); }

// String utilities implementation

std::string CssParser::normalized(const std::string& s) {
  std::string result;
  result.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  // Remove trailing space
  while (!result.empty() && (result.back() == ' ' || result.back() == '\n')) {
    result.pop_back();
  }
  return result;
}

void CssParser::normalizedInto(const std::string_view s, std::string& out) {
  out.clear();
  out.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::vector<std::string> CssParser::splitWhitespace(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool inWord = false;

  for (size_t i = 0; i <= s.size(); ++i) {
    const bool isSpace = i == s.size() || isCssWhitespace(s[i]);
    if (isSpace && inWord) {
      parts.push_back(s.substr(start, i - start));
      inWord = false;
    } else if (!isSpace && !inWord) {
      start = i;
      inWord = true;
    }
  }
  return parts;
}

// Property value interpreters
//
// These take a std::string_view and do NOT normalize. Every caller already passes normalized
// text: parseDeclarationIntoStyle fills propValueBuf via normalizedInto(), splitOnChar()
// normalizes each part it emits, and splitWhitespace() splits an already-normalized string on
// single spaces. normalized() is idempotent (lowercase + collapse whitespace + trim), so the
// re-normalization these used to do was provably a no-op that cost one heap string per call.
//
// That waste is not free even though CSS compiles once per book: on a no-compaction heap the
// holes a short-lived string leaves are permanent for the session, so first-open churn is still
// there when a later chapter needs a contiguous block.

CssTextAlign CssParser::interpretAlignment(const std::string_view v) {
  if (v == "inherit") return CssTextAlign::Inherit;
  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string_view v) {
  if (v == "inherit") return CssFontStyle::Inherit;
  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string_view v) {
  // Named values
  if (v == "inherit") return CssFontWeight::Inherit;
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  // Parsed digit-by-digit rather than via strtol so no NUL-terminated copy is needed; the
  // range is 3 digits, so overflow is not a concern once the length is bounded.
  if (v.empty() || v.size() > 4) return CssFontWeight::Normal;
  long numericWeight = 0;
  for (const char c : v) {
    if (c < '0' || c > '9') return CssFontWeight::Normal;  // not a pure number (e.g. "inherit")
    numericWeight = numericWeight * 10 + (c - '0');
  }
  return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string_view v) {
  // text-decoration can have multiple space-separated values
  const bool underline = v.find("underline") != std::string_view::npos;
  const bool lineThrough = v.find("line-through") != std::string_view::npos;
  uint8_t result = 0;
  if (underline) result |= static_cast<uint8_t>(CssTextDecoration::Underline);
  if (lineThrough) result |= static_cast<uint8_t>(CssTextDecoration::LineThrough);
  return static_cast<CssTextDecoration>(result);
}

CssLength CssParser::interpretLength(const std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(const std::string_view v, CssLength& out) {
  // Was three heap strings per call — normalized(val) plus a substr for each of the number and
  // unit halves — on a function invoked for every margin/padding/indent in the sheet (and four
  // times over for each shorthand). Now zero: the input is already normalized (see the note
  // above the interpreters) and both halves are views into it.
  if (v.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string_view numPart = v.substr(0, unitStart);
  const std::string_view unitPart = v.substr(unitStart);

  // strtof needs a NUL-terminated buffer and string_view gives no guarantee of one. The number
  // half of a CSS length is short by construction, so copy it into a small stack buffer rather
  // than heap-allocating; anything longer than the buffer is not a valid length anyway.
  char numBuf[24];
  if (numPart.empty() || numPart.size() >= sizeof(numBuf)) {
    out = CssLength{};
    return false;
  }
  std::memcpy(numBuf, numPart.data(), numPart.size());
  numBuf[numPart.size()] = '\0';

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numBuf, &endPtr);
  if (endPtr == numBuf) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  auto unit = CssUnit::Pixels;
  if (unitPart == "em") {
    unit = CssUnit::Em;
  } else if (unitPart == "rem") {
    unit = CssUnit::Rem;
  } else if (unitPart == "pt") {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

bool CssParser::interpretImageSize(const std::string_view v, CssLength& out) {
  if (tryInterpretLength(v, out)) return true;

  // `auto` has to CLEAR a length set earlier in the cascade, not be dropped as unparseable.
  // `img { height: 100%; ...; height: auto; width: 100% }` is a common EPUB idiom (it is what
  // sent us here): ignoring the `auto` left the 100% standing, so a wide picture got an image
  // box the full height of the viewport and only the top of it was ever drawn.
  // inherit is excluded on purpose: width/height are not inherited properties, so it is not
  // equivalent to auto and we keep ignoring it.
  if (v == "auto" || v == "initial" || v == "unset" || v == "revert") {
    out = CssLength{0.0f, CssUnit::Auto};
    return true;
  }
  return false;
}

// Declaration parsing

namespace {
// "0", "0.0", ".0", "0%" -> true. No strtof: it is locale-sensitive and this only has to recognise
// zero. Leading/trailing spaces tolerated because the value inside rgba( ... ) is not re-normalised.
bool isZeroNumber(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '%')) s.remove_suffix(1);
  if (s.empty()) return false;
  bool sawDigit = false;
  for (const char c : s) {
    if (c == '0')
      sawDigit = true;
    else if (c != '.')
      return false;
  }
  return sawDigit;
}

// A colour value that paints nothing: the keyword, an rgba()/hsla() with alpha 0, or a hex
// colour with an alpha channel of 00 (#rrggbb00, #rgb0). Six-digit hex has no alpha, so
// #000000 is opaque black even though it ends in "00". Values are already lowercased.
bool isTransparentColour(const std::string_view v) {
  if (v == "transparent") return true;
  if ((v.rfind("rgba(", 0) == 0 || v.rfind("hsla(", 0) == 0) && v.back() == ')') {
    const size_t lastComma = v.rfind(',');
    if (lastComma == std::string_view::npos) return false;
    return isZeroNumber(v.substr(lastComma + 1, v.size() - lastComma - 2));
  }
  if (!v.empty() && v.front() == '#') {
    if (v.size() == 9) return v[7] == '0' && v[8] == '0';
    if (v.size() == 5) return v[4] == '0';
  }
  return false;
}
// A (page-)break-before/after value: 1 forces a page break, 0 asks for none -- auto, avoid,
// avoid-page -- and is kept, so a more specific or later rule can cancel an `always` (a
// `.chapter.first { page-break-before: avoid }` under `.chapter { page-break-before: always }`),
// -1 is neither (column and region breaks, cascade keywords) and leaves the property alone.
int interpretPageBreak(const std::string_view v) {
  if (v == "always" || v == "page" || v == "left" || v == "right" || v == "recto" || v == "verso") return 1;
  if (v == "auto" || v == "avoid" || v == "avoid-page") return 0;
  return -1;
}
}  // namespace

void CssParser::parseDeclarationIntoStyle(const std::string_view decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  // substr on a string_view is a view, not a copy: the two halves are normalized straight into
  // the caller's scratchpad, so a declaration costs zero heap blocks once those have grown.
  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  // Strip `!important` once, here, for every property. It used to be applied per-property at
  // the dozen sites that remembered to, so `margin: 0 !important`, `text-align: center
  // !important`, `text-indent`, `font-style`, `font-weight` and `text-decoration` all parsed
  // the marker as part of the value and silently dropped the declaration.
  // Ported from crosspoint-reader PR #3221 (Phạm Bình An / @brianhuster).
  // stripTrailingImportant only removes suffixes, so the view still starts at propValueBuf's
  // own buffer and resizing to its length is safe.
  propValueBuf.resize(stripTrailingImportant(propValueBuf).size());
  if (propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "font-family") {
    style.fontFamily = firstFontFamily(propValueBuf);
    style.defined.fontFamily = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    // `inherit` names no line, and read as `none` it cancelled an underline the element sits in.
    // It is left unset instead: the element keeps what it inherits.
    if (propValueBuf != "inherit") {
      style.textDecoration = interpretDecoration(propValueBuf);
      style.defined.textDecoration = 1;
    }
  } else if (propNameBuf == "text-indent") {
    style.textIndent = interpretLength(propValueBuf);
    style.defined.textIndent = 1;
  } else if (propNameBuf == "margin-top") {
    style.marginTop = interpretLength(propValueBuf);
    style.defined.marginTop = 1;
  } else if (propNameBuf == "margin-bottom") {
    style.marginBottom = interpretLength(propValueBuf);
    style.defined.marginBottom = 1;
  } else if (propNameBuf == "margin-left") {
    style.marginLeft = interpretLength(propValueBuf);
    style.defined.marginLeft = 1;
  } else if (propNameBuf == "margin-right") {
    style.marginRight = interpretLength(propValueBuf);
    style.defined.marginRight = 1;
  } else if (propNameBuf == "margin") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.marginTop = interpretLength(values[0]);
      style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
      style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
      style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (propNameBuf == "padding-top") {
    style.paddingTop = interpretLength(propValueBuf);
    style.defined.paddingTop = 1;
  } else if (propNameBuf == "padding-bottom") {
    style.paddingBottom = interpretLength(propValueBuf);
    style.defined.paddingBottom = 1;
  } else if (propNameBuf == "padding-left") {
    style.paddingLeft = interpretLength(propValueBuf);
    style.defined.paddingLeft = 1;
  } else if (propNameBuf == "padding-right") {
    style.paddingRight = interpretLength(propValueBuf);
    style.defined.paddingRight = 1;
  } else if (propNameBuf == "padding") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.paddingTop = interpretLength(values[0]);
      style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
      style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
      style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (propNameBuf == "height") {
    CssLength len;
    if (interpretImageSize(propValueBuf, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (propNameBuf == "width") {
    CssLength len;
    if (interpretImageSize(propValueBuf, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (propNameBuf == "color" || propNameBuf == "-webkit-text-fill-color") {
    // Only the INVISIBLE value defines the property. A visible colour UNDEFINES it instead of
    // defining it false, for two reasons: a rule that only sets `color: red` still counts as
    // "nothing we use" and stays out of the rule cache (see the SkipsRulesWithoutSupportedDeclarations
    // test -- on a monochrome panel every book's colour rules would otherwise be retained for
    // nothing); and within one declaration list `color: transparent; color: black` still ends
    // visible, because black clears what transparent set. Across separate rules a later visible
    // colour cannot cancel an earlier transparent one -- accepted: OCR layers do not toggle
    // transparency by class. Cascade keywords (inherit/initial/unset/currentcolor) touch nothing.
    const std::string_view val = propValueBuf;
    if (isTransparentColour(val)) {
      style.colorTransparent = true;
      style.defined.colorTransparent = 1;
    } else if (val != "inherit" && val != "initial" && val != "unset" && val != "currentcolor") {
      style.colorTransparent = false;
      style.defined.colorTransparent = 0;
    }
  } else if (propNameBuf == "visibility") {
    // Same convention as color: hidden/collapse define, visible undefines.
    const std::string_view val = propValueBuf;
    if (val == "hidden" || val == "collapse") {
      style.visibilityHidden = true;
      style.defined.visibilityHidden = 1;
    } else if (val == "visible") {
      style.visibilityHidden = false;
      style.defined.visibilityHidden = 0;
    }
  } else if (propNameBuf == "opacity") {
    // Only fully transparent is invisible; 0.5 is dim and stays -- and, per the convention
    // above, a non-zero number undefines rather than defining false. A value that is not a plain
    // number (calc(), var()) leaves the property alone rather than guessing.
    const std::string_view val = propValueBuf;
    if (isZeroNumber(val)) {
      style.opacityZero = true;
      style.defined.opacityZero = 1;
    } else if (!val.empty() && (val.front() == '.' || (val.front() >= '0' && val.front() <= '9'))) {
      style.opacityZero = false;
      style.defined.opacityZero = 0;
    }
  } else if (propNameBuf == "display") {
    // Block-level values only; inline-block, table-cell, contents and the rest stay Inline, the
    // layout a <span> already gets.
    const std::string_view displayValue = propValueBuf;
    if (displayValue == "none") {
      style.display = CssDisplay::None;
    } else if (displayValue == "block" || displayValue == "list-item" || displayValue == "table" ||
               displayValue == "flex" || displayValue == "grid" || displayValue == "flow-root") {
      style.display = CssDisplay::Block;
    } else {
      style.display = CssDisplay::Inline;
    }
    style.defined.display = 1;
  } else if (propNameBuf == "vertical-align") {
    const std::string_view va = propValueBuf;
    if (va == "super") {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (va == "sub") {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    } else if (va == "baseline") {
      style.verticalAlign = CssVerticalAlign::Baseline;
      style.defined.verticalAlign = 1;
    }
  } else if (propNameBuf == "font-variant" || propNameBuf == "font-variant-caps") {
    const std::string_view val = propValueBuf;
    if (val == "small-caps" || val == "all-small-caps") {
      style.smallCaps = true;
      style.defined.smallCaps = 1;
    } else if (val == "normal" || val == "none") {
      style.smallCaps = false;
      style.defined.smallCaps = 1;
    }
  } else if (propNameBuf == "float") {
    const std::string_view val = propValueBuf;
    if (val == "left") {
      style.cssFloat = CssFloat::Left;
      style.defined.cssFloat = 1;
    } else if (val == "right") {
      style.cssFloat = CssFloat::Right;
      style.defined.cssFloat = 1;
    } else if (val == "none") {
      style.cssFloat = CssFloat::None;
      style.defined.cssFloat = 1;
    }
  } else if (propNameBuf == "list-style-type" || propNameBuf == "list-style") {
    const std::string_view val = propValueBuf;
    if (val == "none") {
      style.listStyleNone = true;
      style.defined.listStyleNone = 1;
    }
  } else if (propNameBuf == "page-break-before" || propNameBuf == "break-before") {
    const int forced = interpretPageBreak(propValueBuf);
    if (forced >= 0) {
      style.pageBreakBefore = forced == 1;
      style.defined.pageBreakBefore = 1;
    }
  } else if (propNameBuf == "page-break-after" || propNameBuf == "break-after") {
    const int forced = interpretPageBreak(propValueBuf);
    if (forced >= 0) {
      style.pageBreakAfter = forced == 1;
      style.defined.pageBreakAfter = 1;
    }
  } else if (propNameBuf == "line-height") {
    const std::string_view val = propValueBuf;
    if (val != "normal" && val != "inherit" && val != "initial" && val != "unset") {
      // Parse unitless, %, or em. Normalise to a multiplier relative to default y_advance.
      // Base = 1.5 (typical body line-height). Result range clamped to [0.7, 2.0].
      static constexpr float kBase = 1.5f;
      float parsed = 0.0f;
      bool ok = false;
      if (!val.empty() && val.back() == '%') {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 100.0f / kBase;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "em") {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / kBase;
          ok = true;
        }
      } else {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p && *end == '\0') {
          parsed = v / kBase;
          ok = true;
        }
      }
      if (ok && parsed > 0.0f) {
        style.lineHeightMultiplier = std::max(0.7f, std::min(2.0f, parsed));
        style.defined.lineHeight = 1;
      }
    }
  } else if (propNameBuf == "font-size") {
    const std::string_view val = propValueBuf;
    if (val != "inherit" && val != "initial" && val != "unset") {
      float parsed = 0.0f;
      bool ok = false;
      // em, % and smaller/larger scale the parent's size; everything else is the body's (rem) or a
      // fixed size, normalised against the body's nominal one -- CssStyle::fontSizeRootRelative.
      bool fromRoot = true;
      if (!val.empty() && val.back() == '%') {
        fromRoot = false;
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 100.0f;
          ok = true;
        }
      } else if (val.size() > 3 && val.substr(val.size() - 3) == "rem") {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "em") {
        fromRoot = false;
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "pt") {
        // Absolute points, normalised against a 12 pt nominal body size — the
        // convention print-derived EPUBs assume (CSS medium == 16 px == 12 pt).
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 12.0f;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "px") {
        // Absolute pixels, normalised against the CSS default body size of 16 px.
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 16.0f;
          ok = true;
        }
      } else {
        // CSS absolute-size / relative-size keywords, mapped to the same multiplier
        // steps microreader uses (smaller/larger fold onto small/large).
        if (val == "xx-small") {
          parsed = 0.6f;
        } else if (val == "x-small") {
          parsed = 0.75f;
        } else if (val == "small" || val == "smaller") {
          parsed = 0.8f;
        } else if (val == "medium") {
          parsed = 1.0f;
        } else if (val == "large" || val == "larger") {
          parsed = 1.2f;
        } else if (val == "x-large") {
          parsed = 1.4f;
        } else if (val == "xx-large") {
          parsed = 1.6f;
        }
        ok = parsed > 0.0f;
        fromRoot = val != "smaller" && val != "larger";
      }
      if (ok && parsed > 0.0f) {
        style.fontSizeMultiplier = parsed;
        style.fontSizeRootRelative = fromRoot;
        style.defined.fontSizeMultiplier = 1;
      }
    }
  }
}

CssStyle CssParser::parseDeclarations(const std::string_view declBlock) {
  // One scratchpad for the whole process, not two fresh std::strings per rule block. This is a
  // static function called from two hot paths — every rule block during a compile, and every
  // inline style="" during a render — so the buffers used to be built and destroyed thousands
  // of times. Function-local statics keep their capacity for the life of the process: after the
  // first few blocks these never allocate again.
  //
  // Safe as statics because the parser is single-threaded (parse runs on the build/render task,
  // never both at once) and neither buffer outlives the call.
  static std::string propNameBuf;
  static std::string propValueBuf;

  CssStyle style;
  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        // A view, not a substr copy — parseDeclarationIntoStyle slices it further and
        // normalizes straight into the scratchpad, so the declaration text is never
        // materialised on the heap at all.
        const std::string_view decl = declBlock.substr(start, i - start);
        if (!decl.empty()) {
          parseDeclarationIntoStyle(decl, style, propNameBuf, propValueBuf);
        }
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(const std::string_view selectorGroup, const CssStyle& style) {
  // Inspired by crosspoint-reader#2604: a selector with no renderer-supported
  // declarations only consumes scarce rule-map and cache-index capacity.
  if (!style.defined.anySet()) {
    return;
  }

  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors.
  //
  // Was splitOnChar(), which materialised a std::vector<std::string> of every selector in the
  // group — one heap string per selector, each of them ALREADY normalized inside the split —
  // and then normalized each one a second time into `key` below. For a 119-candidate sheet that
  // is ~240 short-lived heap blocks whose holes outlive the parse: the heap never compacts, so
  // first-open CSS churn is still fragmenting the address space when a later chapter needs a
  // contiguous block.
  //
  // Now: walk the group in place and normalize each part straight into one reused buffer. Zero
  // allocations per selector after the buffer's first growth.
  //
  // Every selector of the group is the same rule, so they share one source order; the counter
  // moves on once the group stored something.
  const uint16_t order = nextRuleOrder_;
  bool stored = false;
  size_t partStart = 0;
  for (size_t i = 0; i <= selectorGroup.size(); ++i) {
    if (i != selectorGroup.size() && selectorGroup[i] != ',') continue;
    const std::string_view rawPart(selectorGroup.data() + partStart, i - partStart);
    partStart = i + 1;

    normalizedInto(rawPart, selectorKeyBuf_);
    if (selectorKeyBuf_.empty()) continue;  // splitOnChar dropped empties too

    totalSelectorCandidates_++;
    // Validate selector length before processing
    if (selectorKeyBuf_.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", selectorKeyBuf_.size(), MAX_SELECTOR_LENGTH);
      unsupportedSelectorSkips_++;
      continue;
    }

    // `*.a` and `*#id` are `.a` and `#id`: the universal selector adds nothing to a compound,
    // not even specificity.
    if (selectorKeyBuf_.size() > 1 && selectorKeyBuf_[0] == '*' &&
        (selectorKeyBuf_[1] == '.' || selectorKeyBuf_[1] == '#')) {
      selectorKeyBuf_.erase(0, 1);
    }

    // Already normalized into selectorKeyBuf_ above — no second pass, no second allocation.
    const std::string& key = selectorKeyBuf_;

    if (!isSelectorUsableByResolver(key)) {
      if (addComplexRule(key, style, order)) {
        stored = true;
      } else {
        unsupportedSelectorSkips_++;
      }
      continue;
    }

    // Record whether ANY usable rule matches on an id. resolveStyle does two extra lookups per
    // id-bearing element ("#id" and "tag#id"); in a sheet with no id selectors both are
    // guaranteed to miss, and the caller can ignore the element's id entirely -- which lets one
    // cached style serve every element of the same tag and class instead of one per id. Checked
    // here, on the normalized selector, so it costs one character scan per rule at parse time.
    if (key.find('#') != std::string::npos) hasIdSelectors_ = true;

    // Skip if this would exceed the rule limit
    const size_t ruleCount =
        compileModeActive_ ? compileSelectorOffsets_.size() + compileRetiredOffsets_.size() : rulesBySelector_.size();
    if (ruleCount >= MAX_RULES) {
      // The cache keeps the first MAX_RULES rules and is stamped truncated (flags bit 1), so
      // the book is parsed once and every open logs what is missing. The compile used to fail
      // here and write no cache at all, which re-parsed the book -- and hit this same cap --
      // on every open: the styles were lost either way, only slower (audit R4).
      if (!rulesTruncated_) {
        LOG_ERR("CSS", "Reached max rules limit (%zu); the rules past it are dropped and the cache is marked truncated",
                MAX_RULES);
      }
      rulesTruncated_ = true;
      return;
    }

    // A selector declared again merges into its earlier rule only when no other rule was declared
    // in between (the same rule block, or the next one). Otherwise the earlier declarations keep
    // their place: merged, they would take the later position and beat an intervening rule of
    // equal specificity they should lose to (`.a{x} .b{y} .a{z}` on class="a b": y beats x).
    // The adjacency rule is crosspoint-reader PR #3891's (Justin Mitchell, @itsthisjustin).
    if (compileModeActive_) {
      if (!compileTempFile_) {
        compileModeFailed_ = true;
        continue;
      }

      compileTempFile_.flush();
      CssStyle merged = style;
      auto existingIt = compileSelectorOffsets_.find(key);
      if (existingIt != compileSelectorOffsets_.end() && existingIt->second.order + 1 >= order) {
        CssStyle existing;
        FsFile tempRead;
        if (Storage.openFileForRead("CSS", compileTempPath_, tempRead) &&
            tempRead.seek(existingIt->second.offset + sizeof(uint16_t) + key.size() + CSS_RULE_ORDER_BYTES) &&
            readCssStylePayload(tempRead, existing)) {
          existing.applyOver(merged);
          merged = existing;
        } else {
          LOG_ERR("CSS", "Failed to read compiled style for selector '%s' at offset %u", key.c_str(),
                  existingIt->second.offset);
        }
        if (tempRead) {
          tempRead.close();
        }
      } else if (existingIt != compileSelectorOffsets_.end()) {
        compileRetiredOffsets_.push_back(existingIt->second.offset);
      }

      const StagedRule staged{static_cast<uint32_t>(compileTempFile_.position()), order};
      writeRuleRecord(compileTempFile_, key, order, merged);
      if (existingIt != compileSelectorOffsets_.end()) {
        existingIt->second = staged;
      } else {
        compileSelectorOffsets_.emplace(key, staged);
      }
      stored = true;
      continue;
    }

    // Store or merge with existing (non-compile mode), by the same rule as above.
    auto range = rulesBySelector_.equal_range(key);
    auto latest = range.second;
    for (auto it = range.first; it != range.second; ++it) {
      if (latest == range.second || it->second.order > latest->second.order) latest = it;
    }
    if (latest != range.second && latest->second.order + 1 >= order) {
      latest->second.style.applyOver(style);
      latest->second.order = order;
    } else {
      rulesBySelector_.emplace(key, OrderedStyle{style, order});
    }
    stored = true;
  }
  if (stored && nextRuleOrder_ < UINT16_MAX) ++nextRuleOrder_;
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;
  // Keep these as std::string since they're passed by reference to parseDeclarationIntoStyle
  std::string propNameBuf;
  std::string propValueBuf;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;
  // Whether the at-rule is an @font-face, whose body is collected into declBuffer (as its prelude,
  // "font-face" or "media screen", was before the brace). Ported from crosspoint-reader PR #3934
  // (Justin Mitchell, @itsthisjustin).
  bool inFontFace = false;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;
  // The rule's background picture, gathered across its declarations (background-image and
  // background-repeat may be separate) and recorded when the rule closes. See backgroundImageFor.
  BackgroundDeclaration ruleBackground;
  int8_t ruleBackgroundRepeat = -1;  // -1 unset, 0 repeats, 1 no-repeat
  const auto noteBackground = [&](const std::string_view decl) {
    const BackgroundDeclaration d = parseBackground(decl);
    if (d.declared) {
      ruleBackground.declared = true;
      ruleBackground.url = d.url;
    }
    if (d.noRepeat) {
      ruleBackgroundRepeat = 1;
    } else if (decl.find("repeat") != std::string_view::npos) {
      ruleBackgroundRepeat = 0;  // repeat, repeat-x, repeat-y, space, round
    }
  };

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        if (++atDepth == 1) {
          inFontFace = equalsIgnoreCase(trimCss(declBuffer.view()), "font-face");
          declBuffer.clear();
        }
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) {
          if (inFontFace && !declBuffer.overflowed) parseFontFace(declBuffer.view());
          inFontFace = false;
          declBuffer.clear();
          inAtRule = false;
        }
      } else if (c == ';' && atDepth == 0) {
        declBuffer.clear();
        inAtRule = false;
      } else if (atDepth == 0 || (inFontFace && atDepth == 1)) {
        declBuffer.push_back(c);
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        declBuffer.clear();  // the prelude gathers here
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        ruleBackground = BackgroundDeclaration{};
        ruleBackgroundRepeat = -1;
        declBuffer.clear();
        // A selector group that overflowed the StackBuffer was silently truncated; the
        // truncated tail could otherwise be parsed as a bogus rule (e.g. a cut class name
        // accidentally matching a real one). Skip the entire rule instead.
        if (selector.overflowed) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        // A truncated (overflowed) trailing declaration is dropped rather than parsed as garbage.
        if (!skippingRule && !declBuffer.empty() && !declBuffer.overflowed) {
          parseDeclarationIntoStyle(declBuffer.view(), currentStyle, propNameBuf, propValueBuf);
          noteBackground(declBuffer.view());
        }
        if (!skippingRule) {
          // Before processRuleBlockWithStyle, which drops a rule with no style declaration: a rule
          // that only sets a background picture is exactly such a rule.
          if (ruleBackground.declared) {
            const bool picture = !ruleBackground.url.empty() && ruleBackgroundRepeat == 1;
            recordBackgroundImage(selector.view(), picture ? stylesheetDir_ + ruleBackground.url : std::string());
          }
          processRuleBlockWithStyle(selector.view(), currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        // clear() also resets the overflow flag, so a single oversized declaration
        // is dropped without poisoning the declarations that follow it in the block.
        if (!declBuffer.empty() && !declBuffer.overflowed) {
          parseDeclarationIntoStyle(declBuffer.view(), currentStyle, propNameBuf, propValueBuf);
          noteBackground(declBuffer.view());
        }
        declBuffer.clear();
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  if (compileModeActive_) {
    LOG_DBG("CSS", "Parsed %zu usable selectors from %zu bytes (compile mode)", compileSelectorOffsets_.size(),
            totalRead);
  } else {
    LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  }
  return true;
}

bool CssParser::beginCacheCompile() {
  clear();
  compileTempPath_ = cachePath + compileTempRulesCache;
  Storage.remove(compileTempPath_.c_str());
  if (!Storage.openFileForWrite("CSS", compileTempPath_, compileTempFile_)) {
    return false;
  }
  compileSelectorOffsets_.clear();
  compileRetiredOffsets_.clear();
  compileModeActive_ = true;
  compileModeFailed_ = false;
  rulesTruncated_ = false;
  return true;
}

bool CssParser::appendCompiledFromStream(FsFile& source) {
  if (!compileModeActive_) {
    return false;
  }
  if (!loadFromStream(source)) {
    compileModeFailed_ = true;
    return false;
  }
  return !compileModeFailed_;
}

void CssParser::abortCacheCompile() {
  if (!compileModeActive_) {
    return;
  }
  // Reuse the failed-compile teardown: closes and removes the temp staging file and leaves no
  // rules cache behind (hasCache() stays false, so the next open re-parses the stylesheets).
  compileModeFailed_ = true;
  endCacheCompile();
}

bool CssParser::endCacheCompile() {
  if (!compileModeActive_) {
    return false;
  }

  compileModeActive_ = false;
  compileTempFile_.close();
  // swap, not clear(): clear() frees the nodes but keeps the bucket array, which would sit
  // under the index ensureCacheIndexLoaded() is about to allocate.
  // The complex-rule table is staged in RAM: dropIndex() releases it (written to the cache by
  // then, or abandoned with the failed compile).
  const auto dropStaging = [this] {
    Storage.remove(compileTempPath_.c_str());
    std::unordered_map<std::string, StagedRule>().swap(compileSelectorOffsets_);
    std::vector<uint32_t>().swap(compileRetiredOffsets_);
    dropIndex();
  };

  if (compileModeFailed_) {
    dropStaging();
    return false;
  }

  FsFile outFile;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, outFile)) {
    dropStaging();
    return false;
  }

  outFile.write(CssParser::CSS_CACHE_VERSION);
  const auto ruleCount = static_cast<uint16_t>(compileSelectorOffsets_.size() + compileRetiredOffsets_.size());
  outFile.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));
  outFile.write(reinterpret_cast<const uint8_t*>(&totalSelectorCandidates_), sizeof(totalSelectorCandidates_));
  outFile.write(reinterpret_cast<const uint8_t*>(&unsupportedSelectorSkips_), sizeof(unsupportedSelectorSkips_));
  outFile.write(static_cast<uint8_t>((hasIdSelectors_ ? 1 : 0) | (rulesTruncated_ ? 2 : 0)));  // v17 flags byte
  writeComplexTable(outFile);
  // v10: index immediately after the header — write zeroed placeholder, patch below.

  FsFile tempFile;
  if (!Storage.openFileForRead("CSS", compileTempPath_, tempFile)) {
    outFile.close();
    Storage.remove((cachePath + rulesCache).c_str());
    dropStaging();
    return false;
  }

  std::vector<SelectorEntry> indexEntries;
  indexEntries.reserve(ruleCount);
  {
    constexpr SelectorEntry kZero{};
    for (uint16_t i = 0; i < ruleCount; ++i) {
      outFile.write(reinterpret_cast<const uint8_t*>(&kZero), sizeof(kZero));
    }
  }

  // The staging records are already in the cache's record format; copy each live one across.
  char selectorBuf[MAX_SELECTOR_LENGTH];
  std::array<uint8_t, CSS_RULE_ORDER_BYTES + CSS_FIXED_STYLE_BYTES> tail{};
  const auto copyRecord = [&](const uint32_t stagedOffset) {
    uint16_t selectorLen = 0;
    if (!tempFile.seek(stagedOffset) || tempFile.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) ||
        selectorLen > MAX_SELECTOR_LENGTH || tempFile.read(selectorBuf, selectorLen) != selectorLen ||
        tempFile.read(tail.data(), tail.size()) != static_cast<int>(tail.size())) {
      return false;
    }
    const uint32_t ruleOffset = outFile.position();
    outFile.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    outFile.write(reinterpret_cast<const uint8_t*>(selectorBuf), selectorLen);
    outFile.write(tail.data(), tail.size());
    indexEntries.push_back({selectorHash(std::string_view(selectorBuf, selectorLen)), ruleOffset});
    return true;
  };
  bool copied = true;
  for (const auto& it : compileSelectorOffsets_) copied = copied && copyRecord(it.second.offset);
  for (const uint32_t offset : compileRetiredOffsets_) copied = copied && copyRecord(offset);
  tempFile.close();
  if (!copied) {
    outFile.close();
    Storage.remove((cachePath + rulesCache).c_str());
    dropStaging();
    return false;
  }

  // Sort and patch the index placeholder at position 11.
  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  outFile.seek(CssParser::CSS_CACHE_HEADER_BYTES + complexBytes_);
  for (const auto& entry : indexEntries) {
    outFile.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
  }

  outFile.close();
  saveBackgroundImages();
  backgroundImagesLoaded_ = true;  // what was just compiled is what the file now holds
  saveFontFaces();
  fontFaces_.clear();  // read back from the side file by whoever needs them
  fontFaces_.shrink_to_fit();
  fontFaceCount_ = 0;
  dropStaging();

  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  dropIndex();

  return ensureCacheIndexLoaded();
}

bool CssParser::empty() const { return ruleCount() == 0; }

size_t CssParser::ruleCount() const {
  if (!rulesBySelector_.empty()) {
    return rulesBySelector_.size();
  }
  if (cacheIndexLoaded_) {
    return cachedRuleCount_;
  }
  return 0;
}

void CssParser::clearCaches(const bool evictEverything) {
  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  // Arena-backed ruleset (resident or index): the memory belongs to the build arena, which is
  // reset per build, so never try to retain it across sections — drop the view and reload from
  // disk on the next resolve (the arena is re-filled fresh). The 10 KB heap-retain heuristic
  // below is meaningless here (nothing is on the heap).
  if (indexArena_) {
    dropIndex();
    return;
  }
  if (evictEverything) {
    // Defragmentation eviction: swap the unordered containers down so their bucket
    // arrays are freed too (clear() keeps them), and drop the retained disk index.
    // Everything here reloads lazily from SD on the next resolve.
    std::unordered_multimap<std::string, OrderedStyle>().swap(rulesBySelector_);
    std::unordered_map<std::string, std::pair<OrderedStyle, std::list<std::string>::iterator>>().swap(hotRuleCache_);
    std::unordered_set<std::string>().swap(negativeRuleCache_);
    dropIndex();
    return;
  }
  // Retain the sorted disk index if it fits in 10 KB — avoids a cold SD re-read
  // (~240 ms) at the start of each section build. Evict if larger to protect heap.
  if (cacheRuleOffsets_.size() * CSS_INDEX_BYTES_PER_RULE > 10 * 1024) {
    dropIndex();
  }
}

void CssParser::clear() {
  if (compileTempFile_) {
    compileTempFile_.flush();
    compileTempFile_.close();
  }
  if (!compileTempPath_.empty()) {
    Storage.remove(compileTempPath_.c_str());
    compileTempPath_.clear();
  }
  rulesBySelector_.clear();
  dropIndex();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  resolveStats_ = {};
  std::vector<BackgroundImageRule>().swap(backgroundImages_);
  backgroundImagesLoaded_ = false;  // reread from the side file on the next query
  fontFaces_.clear();
  fontFaces_.shrink_to_fit();
  fontFaceCount_ = 0;
  stylesheetDir_.clear();
  compileModeActive_ = false;
  compileModeFailed_ = false;
  rulesTruncated_ = false;
  std::unordered_map<std::string, StagedRule>().swap(compileSelectorOffsets_);
  std::vector<uint32_t>().swap(compileRetiredOffsets_);
  nextRuleOrder_ = 0;
  totalSelectorCandidates_ = 0;
  unsupportedSelectorSkips_ = 0;
  // Cleared to FALSE, unlike its conservative default: what follows is either a parse, which
  // sets it from the selectors it sees, or a cache load, which restores it from the header.
  hasIdSelectors_ = false;
  // Reset Phase-2 arena config: clear() ends a build, so the shared per-epub parser must not
  // carry the lean flag or a now-dangling arena pointer into the next (possibly heap-backed) one.
  indexArena_ = nullptr;
  arenaLoadAttempted_ = false;
  leanResolve_ = false;
}

void CssParser::resetResolveStats() const { resolveStats_ = {}; }

CssParser::ResolveStats CssParser::getResolveStats() const { return resolveStats_; }

void CssParser::logResolveStats(const char* context) const {
  const auto s = getResolveStats();
  LOG_DBG("CSS",
          "resolve stats[%s]: calls=%lu lowHeapSkips=%lu lowHeapRescuedHits=%lu lowHeapDiskBypasses=%lu "
          "mapHits=%lu hotHits=%lu diskHits=%lu misses=%lu negativeHits=%lu "
          "unsupportedSelectorsSkipped=%lu totalSelectorCandidates=%lu hotSize=%u indexSize=%u",
          context ? context : "n/a", s.resolveCalls, s.lowHeapSkips, s.lowHeapRescuedHits, s.lowHeapDiskBypasses,
          s.mapHits, s.hotHits, s.diskHits, s.misses, s.negativeHits,
          static_cast<unsigned long>(unsupportedSelectorSkips_), static_cast<unsigned long>(totalSelectorCandidates_),
          static_cast<unsigned>(hotRuleCache_.size()), static_cast<unsigned>(cachedRuleCount_));
  (void)context;
  (void)s;
}

bool CssParser::readCssStylePayload(FsFile& file, CssStyle& style) {
  uint8_t enumVal;
  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.textAlign = static_cast<CssTextAlign>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.fontStyle = static_cast<CssFontStyle>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.fontWeight = static_cast<CssFontWeight>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.textDecoration = static_cast<CssTextDecoration>(enumVal);

  auto readLength = [&file](CssLength& len) -> bool {
    if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) {
      return false;
    }
    uint8_t unitVal;
    if (file.read(&unitVal, 1) != 1) {
      return false;
    }
    len.unit = static_cast<CssUnit>(unitVal);
    return true;
  };

  if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
      !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
      !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
      !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
    return false;
  }

  uint8_t displayVal;
  if (file.read(&displayVal, 1) != 1) {
    return false;
  }
  style.display = static_cast<CssDisplay>(displayVal);

  uint16_t definedBits = 0;
  if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
    return false;
  }
  style.defined.textAlign = (definedBits & 1 << 0) != 0;
  style.defined.fontStyle = (definedBits & 1 << 1) != 0;
  style.defined.fontWeight = (definedBits & 1 << 2) != 0;
  style.defined.textDecoration = (definedBits & 1 << 3) != 0;
  style.defined.textIndent = (definedBits & 1 << 4) != 0;
  style.defined.marginTop = (definedBits & 1 << 5) != 0;
  style.defined.marginBottom = (definedBits & 1 << 6) != 0;
  style.defined.marginLeft = (definedBits & 1 << 7) != 0;
  style.defined.marginRight = (definedBits & 1 << 8) != 0;
  style.defined.paddingTop = (definedBits & 1 << 9) != 0;
  style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  style.defined.paddingRight = (definedBits & 1 << 12) != 0;
  style.defined.imageHeight = (definedBits & 1 << 13) != 0;
  style.defined.imageWidth = (definedBits & 1 << 14) != 0;
  style.defined.display = (definedBits & 1 << 15) != 0;
  uint8_t vertAlignVal = 0;
  uint8_t vertAlignDefined = 0;
  if (file.read(&vertAlignVal, 1) != 1 || file.read(&vertAlignDefined, 1) != 1) {
    return false;
  }
  style.verticalAlign = static_cast<CssVerticalAlign>(vertAlignVal);
  style.defined.verticalAlign = vertAlignDefined != 0 ? 1 : 0;
  uint8_t cssFloatVal = 0;
  if (file.read(&cssFloatVal, 1) != 1) {
    return false;
  }
  style.cssFloat = static_cast<CssFloat>(cssFloatVal);
  style.defined.cssFloat = (cssFloatVal != static_cast<uint8_t>(CssFloat::None)) ? 1 : 0;
  uint8_t smallCapsVal = 0;
  if (file.read(&smallCapsVal, 1) != 1) {
    return false;
  }
  // bit 0 = value, bit 1 = defined (distinguishes explicit "normal" from unset)
  style.smallCaps = (smallCapsVal & 0x1) != 0;
  style.defined.smallCaps = (smallCapsVal & 0x2) != 0 ? 1 : 0;
  float fontSizeMul = 1.0f;
  uint8_t fontSizeFlags = 0;
  if (file.read(&fontSizeMul, sizeof(fontSizeMul)) != sizeof(fontSizeMul) || file.read(&fontSizeFlags, 1) != 1) {
    return false;
  }
  if ((fontSizeFlags & 0x1) != 0) {
    style.fontSizeMultiplier = fontSizeMul;
    style.fontSizeRootRelative = (fontSizeFlags & 0x2) != 0;
    style.defined.fontSizeMultiplier = 1;
  }
  style.defined.fontFamily = (fontSizeFlags & 0x4) != 0 ? 1 : 0;
  uint8_t blockFlags = 0;
  if (file.read(&blockFlags, 1) != 1) {
    return false;
  }
  style.listStyleNone = (blockFlags & 0x01) != 0;
  style.defined.listStyleNone = (blockFlags & 0x02) != 0 ? 1 : 0;
  style.pageBreakBefore = (blockFlags & 0x04) != 0;
  style.defined.pageBreakBefore = (blockFlags & 0x08) != 0 ? 1 : 0;
  style.pageBreakAfter = (blockFlags & 0x10) != 0;
  style.defined.pageBreakAfter = (blockFlags & 0x20) != 0 ? 1 : 0;
  uint8_t visibilityFlags = 0;
  if (file.read(&visibilityFlags, 1) != 1) {
    return false;
  }
  style.colorTransparent = (visibilityFlags & 0x01) != 0;
  style.defined.colorTransparent = (visibilityFlags & 0x02) != 0 ? 1 : 0;
  style.opacityZero = (visibilityFlags & 0x04) != 0;
  style.defined.opacityZero = (visibilityFlags & 0x08) != 0 ? 1 : 0;
  style.visibilityHidden = (visibilityFlags & 0x10) != 0;
  style.defined.visibilityHidden = (visibilityFlags & 0x20) != 0 ? 1 : 0;
  return file.read(&style.fontFamily, sizeof(style.fontFamily)) == sizeof(style.fontFamily);
}

void CssParser::writeCssStylePayload(FsFile& file, const CssStyle& style) {
  file.write(static_cast<uint8_t>(style.textAlign));
  file.write(static_cast<uint8_t>(style.fontStyle));
  file.write(static_cast<uint8_t>(style.fontWeight));
  file.write(static_cast<uint8_t>(style.textDecoration));

  auto writeLength = [&file](const CssLength& len) {
    file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
    file.write(static_cast<uint8_t>(len.unit));
  };

  writeLength(style.textIndent);
  writeLength(style.marginTop);
  writeLength(style.marginBottom);
  writeLength(style.marginLeft);
  writeLength(style.marginRight);
  writeLength(style.paddingTop);
  writeLength(style.paddingBottom);
  writeLength(style.paddingLeft);
  writeLength(style.paddingRight);
  writeLength(style.imageHeight);
  writeLength(style.imageWidth);
  file.write(static_cast<uint8_t>(style.display));

  uint16_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  file.write(static_cast<uint8_t>(style.verticalAlign));
  file.write(static_cast<uint8_t>(style.defined.verticalAlign));
  file.write(static_cast<uint8_t>(style.cssFloat));
  // bit 0 = value, bit 1 = defined (distinguishes explicit "normal" from unset)
  uint8_t smallCapsVal = (style.smallCaps ? 0x1 : 0x0) | (style.defined.smallCaps ? 0x2 : 0x0);
  file.write(smallCapsVal);
  // font-size multiplier: float + flags byte (bit 0 = defined, bit 1 = relative to the body, v27;
  // bit 2 = font-family defined, v28)
  file.write(reinterpret_cast<const uint8_t*>(&style.fontSizeMultiplier), sizeof(style.fontSizeMultiplier));
  const uint8_t fontSizeFlags = (style.defined.fontSizeMultiplier ? 0x1 : 0x0) |
                                (style.fontSizeRootRelative ? 0x2 : 0x0) | (style.defined.fontFamily ? 0x4 : 0x0);
  file.write(fontSizeFlags);
  // Block flags byte: value/defined pairs, same convention as smallCaps above.
  const uint8_t blockFlags = (style.listStyleNone ? 0x01 : 0x00) | (style.defined.listStyleNone ? 0x02 : 0x00) |
                             (style.pageBreakBefore ? 0x04 : 0x00) | (style.defined.pageBreakBefore ? 0x08 : 0x00) |
                             (style.pageBreakAfter ? 0x10 : 0x00) | (style.defined.pageBreakAfter ? 0x20 : 0x00);
  file.write(blockFlags);
  // Visibility flags byte (v19): value/defined pairs, same convention.
  const uint8_t visibilityFlags = (style.colorTransparent ? 0x01 : 0x00) |
                                  (style.defined.colorTransparent ? 0x02 : 0x00) | (style.opacityZero ? 0x04 : 0x00) |
                                  (style.defined.opacityZero ? 0x08 : 0x00) | (style.visibilityHidden ? 0x10 : 0x00) |
                                  (style.defined.visibilityHidden ? 0x20 : 0x00);
  file.write(visibilityFlags);
  file.write(reinterpret_cast<const uint8_t*>(&style.fontFamily), sizeof(style.fontFamily));
}

void CssParser::writeComplexTable(FsFile& file) const {
  file.write(reinterpret_cast<const uint8_t*>(&complexBytes_), sizeof(complexBytes_));
  if (complexBytes_ > 0) file.write(complexTable(), complexBytes_);
}

void CssParser::writeRuleRecord(FsFile& file, const std::string& selector, const uint16_t order,
                                const CssStyle& style) {
  const auto selectorLen = static_cast<uint16_t>(selector.size());
  file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
  file.write(reinterpret_cast<const uint8_t*>(selector.data()), selectorLen);
  file.write(reinterpret_cast<const uint8_t*>(&order), sizeof(order));
  writeCssStylePayload(file, style);
}

void CssParser::touchHotRule(const std::string& selector) const {
  auto it = hotRuleCache_.find(selector);
  if (it == hotRuleCache_.end()) {
    return;
  }
  hotRuleLru_.erase(it->second.second);
  hotRuleLru_.push_front(selector);
  it->second.second = hotRuleLru_.begin();
}

void CssParser::cacheHotRule(const std::string& selector, const OrderedStyle& rule) const {
  // Lean/arena builds skip the hot LRU entirely: its ~100 B/entry heap growth is exactly what
  // the CSS floor guarded against, and arena builds resolve from the resident/index store
  // instead (FreeInkBook keeps no such cache). This is a no-op call from the arena index-only
  // path; it is never reached in RESIDENT mode.
  if (leanResolve_) {
    return;
  }
  auto it = hotRuleCache_.find(selector);
  if (it != hotRuleCache_.end()) {
    it->second.first = rule;
    touchHotRule(selector);
    return;
  }

  hotRuleLru_.push_front(selector);
  hotRuleCache_.emplace(selector, std::make_pair(rule, hotRuleLru_.begin()));
  if (hotRuleCache_.size() > HOT_RULE_CACHE_SIZE) {
    const std::string& evictKey = hotRuleLru_.back();
    hotRuleCache_.erase(evictKey);
    hotRuleLru_.pop_back();
  }
}

// Reads the rule record at ruleOffset and returns its style only if the stored
// selector matches `selector` exactly. A mismatch is not an error — it means the
// 32-bit index hash collided and the caller should try the next candidate.
bool CssParser::readRuleFromDiskAtOffset(const uint32_t ruleOffset, const std::string& selector, CssStyle& outStyle,
                                         uint16_t& outOrder) const {
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // 256 B stack buffer (bounded by MAX_SELECTOR_LENGTH): render-path code, so no
  // heap; not live at the same time as the index-load buffer — both are reached
  // sequentially from lookupRules, so peak stack on this chain stays ~256 B.
  char selectorBuf[MAX_SELECTOR_LENGTH];
  uint16_t selectorLen = 0;
  bool ok = file.seek(ruleOffset) && file.read(&selectorLen, sizeof(selectorLen)) == sizeof(selectorLen) &&
            selectorLen == selector.size() && selectorLen <= MAX_SELECTOR_LENGTH &&
            file.read(selectorBuf, selectorLen) == selectorLen &&
            std::string_view(selectorBuf, selectorLen) == selector;
  ok = ok && file.read(&outOrder, sizeof(outOrder)) == sizeof(outOrder) && readCssStylePayload(file, outStyle);
  file.close();
  return ok;
}

struct CssParser::Cascade {
  CssStyle result;
  uint32_t ranks[CssStyle::kCascadeProperties] = {};
  void apply(const CssStyle& style, const uint32_t specificity, const uint16_t order) {
    result.applyOver(style, specificity << 16 | order, ranks);
  }
};

bool CssParser::lookupRules(const std::string& selector, const uint32_t specificity, Cascade& cascade,
                            const bool allowDiskLookup) const {
  // Rules held in RAM from a parse (rather than loaded from the cache).
  if (!rulesBySelector_.empty()) {
    const auto range = rulesBySelector_.equal_range(selector);
    for (auto it = range.first; it != range.second; ++it) {
      cascade.apply(it->second.style, specificity, it->second.order);
      resolveStats_.mapHits++;
    }
    if (range.first != range.second) return true;
  }

  auto hotIt = hotRuleCache_.find(selector);
  if (hotIt != hotRuleCache_.end()) {
    cascade.apply(hotIt->second.first.style, specificity, hotIt->second.first.order);
    touchHotRule(selector);
    resolveStats_.hotHits++;
    return true;
  }

  if (negativeRuleCache_.find(selector) != negativeRuleCache_.end()) {
    resolveStats_.negativeHits++;
    return false;
  }

  const auto byHash = [](const SelectorEntry& e, uint32_t key) { return e.hash < key; };
  // Every record of the selector: a selector redeclared further down has one per declaration
  // (see processRuleBlockWithStyle). Equal hashes are adjacent after the sort; each candidate's
  // stored selector is compared, so a 32-bit collision costs a read, not a wrong style.
  const auto readAll = [&](const SelectorEntry* begin, const SelectorEntry* end, const bool cacheIfSingle) {
    const uint32_t h = selectorHash(selector);
    OrderedStyle rule;
    int matched = 0;
    for (auto it = std::lower_bound(begin, end, h, byHash); it != end && it->hash == h; ++it) {
      if (readRuleFromDiskAtOffset(it->offset, selector, rule.style, rule.order)) {
        cascade.apply(rule.style, specificity, rule.order);
        resolveStats_.diskHits++;
        ++matched;
      }
    }
    if (matched == 1 && cacheIfSingle) cacheHotRule(selector, rule);
    if (matched > 0) return true;
    if (negativeRuleCache_.size() >= NEGATIVE_CACHE_SIZE) {
      negativeRuleCache_.clear();
    }
    negativeRuleCache_.insert(selector);
    return false;
  };

  // --- Arena mode (Phase 2) ---
  // The ruleset lives in arena memory (a heap-free fill), so load it even under heap pressure.
  if (indexArena_) {
    if (!ensureCacheIndexLoaded()) {
      return false;
    }

    // RESIDENT: the whole ruleset is in RAM — resolve by hash with no disk read and no caching
    // (FreeInkBook's model). A hash collision between two distinct selectors is astronomically
    // unlikely at these rule counts and costs at most one wrong style; upstream accepts that
    // rather than paying a disk read to verify the selector string, and so do we. No low-heap
    // guard applies — nothing here touches SD or grows the heap.
    if (arenaResident_) {
      const uint32_t h = selectorHash(selector);
      const ResidentEntry* begin = arenaResident_;
      const ResidentEntry* end = arenaResident_ + cachedRuleCount_;
      auto it = std::lower_bound(begin, end, h, [](const ResidentEntry& e, uint32_t key) { return e.hash < key; });
      bool matched = false;
      for (; it != end && it->hash == h; ++it) {
        CssStyle style;
        decompressStyle(arenaStylePool_ + it->styleOff + 1, style);  // +1 skips the length prefix
        cascade.apply(style, specificity, it->order);
        resolveStats_.diskHits++;  // resolved from the (in-RAM) ruleset
        matched = true;
      }
      return matched;  // a miss is a cheap re-scan next time; no negative cache needed in RAM
    }

    // Index-only fallback: payloads are on disk, so honor the low-heap bypass.
    if (!allowDiskLookup) {
      resolveStats_.lowHeapDiskBypasses++;
      return false;
    }
    return readAll(arenaIndex_, arenaIndex_ + cachedRuleCount_, false);
  }

  // --- Heap mode (disk index + hot cache) ---
  if (!allowDiskLookup) {
    resolveStats_.lowHeapDiskBypasses++;
    return false;
  }
  if (!ensureCacheIndexLoaded()) {
    return false;
  }
  return readAll(cacheRuleOffsets_.data(), cacheRuleOffsets_.data() + cacheRuleOffsets_.size(), true);
}

bool CssParser::ensureCacheIndexLoaded() const {
  if (cacheIndexLoaded_) {
    return true;
  }
  if (indexArena_ != nullptr) {
    if (arenaLoadAttempted_) return false;  // see the member: one load per build, at setup
    arenaLoadAttempted_ = true;
  }

  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount) || ruleCount > MAX_RULES) {
    file.close();
    return false;
  }

  uint32_t totalCandidates = 0;
  uint32_t unsupportedSkips = 0;
  uint8_t flags = 0;
  uint16_t complexBytes = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&totalCandidates), sizeof(totalCandidates)) != sizeof(totalCandidates) ||
      file.read(reinterpret_cast<uint8_t*>(&unsupportedSkips), sizeof(unsupportedSkips)) != sizeof(unsupportedSkips) ||
      file.read(&flags, 1) != 1 || file.read(&complexBytes, sizeof(complexBytes)) != sizeof(complexBytes) ||
      complexBytes > MAX_COMPLEX_BYTES) {
    file.close();
    return false;
  }
  hasIdSelectors_ = (flags & 0x01) != 0;
  rulesTruncated_ = (flags & 0x02) != 0;
  if (rulesTruncated_) {
    LOG_ERR("CSS", "This book's stylesheets exceed %zu rules; the rules past the cap are not applied", MAX_RULES);
  }

  // v10: index is immediately after the header (v23: and the complex-rule table) — read
  // sequentially, no seek.
  dropIndex();  // clears the heap vector and any arena view; resets cachedRuleCount_
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();

  // The complex rules are always in RAM (see complexTable): in the arena next to the index, or on
  // the heap. Placed before the index so a failed RESIDENT attempt rewinds only past itself.
  if (complexBytes > 0) {
    uint8_t* table = nullptr;
    if (indexArena_) {
      table = static_cast<uint8_t*>(indexArena_->alloc(complexBytes, 1));
    } else {
      complexHeap_ = makeUniqueNoThrow<uint8_t[]>(complexBytes);
      table = complexHeap_.get();
    }
    if (table == nullptr || file.read(table, complexBytes) != complexBytes || !complexTableValid(table, complexBytes)) {
      LOG_ERR("CSS", "Could not load %u bytes of complex CSS rules", static_cast<unsigned>(complexBytes));
      file.close();
      dropIndex();
      return false;
    }
    if (indexArena_) complexArena_ = table;
    complexBytes_ = complexBytes;
  }

  // Arena mode (Phase 2): prefer the resident {hash, CssStyle} ruleset (in-RAM resolve, no
  // disk reads — see loadArenaResident); fall back to an arena-backed offset index when the
  // resident array won't fit. loadArenaResident leaves the file open for that fallback read.
  if (indexArena_) {
    if (loadArenaResident(file, ruleCount, totalCandidates, unsupportedSkips)) {
      file.close();
      return true;
    }
    // Resident didn't fit — rewind to just past the 11-byte header and read the offset index
    // into the arena instead. Payloads stay on disk (readRuleFromDiskAtOffset).
    const size_t indexBytes = static_cast<size_t>(ruleCount) * sizeof(SelectorEntry);
    if (ruleCount > 0) {
      auto* idx = static_cast<SelectorEntry*>(indexArena_->alloc(indexBytes, alignof(SelectorEntry)));
      if (!idx || !file.seek(CssParser::CSS_CACHE_HEADER_BYTES + complexBytes_) ||
          file.read(reinterpret_cast<uint8_t*>(idx), indexBytes) != static_cast<int>(indexBytes)) {
        file.close();
        return false;  // arena exhausted or short read — caller falls back to a released build
      }
      arenaIndex_ = idx;
    }
    file.close();
    cachedRuleCount_ = ruleCount;
    totalSelectorCandidates_ = totalCandidates;
    unsupportedSelectorSkips_ = unsupportedSkips;
    cacheIndexLoaded_ = true;
    LOG_DBG("CSS", "Loaded CSS index (arena, disk payloads): %u selectors", static_cast<unsigned>(ruleCount));
    return true;
  }

  if (ruleCount > 0) {
    cacheRuleOffsets_.resize(ruleCount);
    const size_t indexBytes = static_cast<size_t>(ruleCount) * sizeof(SelectorEntry);
    if (file.read(reinterpret_cast<uint8_t*>(cacheRuleOffsets_.data()), indexBytes) != static_cast<int>(indexBytes)) {
      file.close();
      dropIndex();
      return false;
    }
  }

  file.close();

  cachedRuleCount_ = cacheRuleOffsets_.size();
  totalSelectorCandidates_ = totalCandidates;
  unsupportedSelectorSkips_ = unsupportedSkips;
  cacheIndexLoaded_ = true;
  LOG_DBG("CSS", "Loaded CSS index: %u selectors (hot cache size=%u, unsupported=%lu/%lu)",
          static_cast<unsigned>(cachedRuleCount_), static_cast<unsigned>(HOT_RULE_CACHE_SIZE),
          static_cast<unsigned long>(unsupportedSelectorSkips_), static_cast<unsigned long>(totalSelectorCandidates_));
  return true;
}

// --- Sparse CssStyle compression for the resident pool ---
// A style is serialized to a canonical, SPARSE record: only the fields the 'defined' mask
// flags are written (a real rule sets a handful of 27 properties). Because the encoding is
// canonical, two equivalent styles produce identical bytes, so dedup is a memcmp — and lookup
// decompresses back to a full CssStyle. Values are copied with memcpy so the byte-packed
// (unaligned) records are safe to read on the C3.

// Worst case: 4 (mask) + 2 (packed enums) + 1 (extra values) + 11 lengths * 5 + 2 multipliers * 4
// + the font-family hash (4) = 74 bytes.
constexpr size_t kMaxCompressedStyle = 4 + 2 + 1 + 11 * 5 + 2 * 4 + 4;

// Canonical "which properties are set" mask: 27 defined bits (0-26), then the three boolean
// VALUES of the invisibility properties in bits 27-29 and the high bit of `display` in bit 30 --
// the uint16 of enum values below is full. (By the CssStyle invariant a defined invisibility
// property is currently always true, so bits 27-29 mirror bits 24-26; they are carried for
// uniformity, not information.) Bit
// order is the pool's dedup key and must stay stable within a build (the pool is rebuilt each
// load, so it never persists to disk).
static uint32_t packDefinedMask(const CssPropertyFlags& d) {
  return (d.textAlign << 0) | (d.fontStyle << 1) | (d.fontWeight << 2) | (d.textDecoration << 3) | (d.textIndent << 4) |
         (d.marginTop << 5) | (d.marginBottom << 6) | (d.marginLeft << 7) | (d.marginRight << 8) | (d.paddingTop << 9) |
         (d.paddingBottom << 10) | (d.paddingLeft << 11) | (d.paddingRight << 12) | (d.imageHeight << 13) |
         (d.imageWidth << 14) | (d.display << 15) | (d.verticalAlign << 16) | (d.listStyleNone << 17) |
         (d.pageBreakBefore << 18) | (d.pageBreakAfter << 19) | (d.lineHeight << 20) | (d.fontSizeMultiplier << 21) |
         (d.cssFloat << 22) | (d.smallCaps << 23) | (d.colorTransparent << 24) | (d.opacityZero << 25) |
         (d.visibilityHidden << 26);
}

// Serialize `s` into `out` (>= kMaxCompressedStyle bytes); returns the record length.
static size_t compressStyle(const CssStyle& s, uint8_t* out) {
  uint8_t* p = out;
  uint32_t mask = packDefinedMask(s.defined);
  if (s.defined.colorTransparent && s.colorTransparent) mask |= 1u << 27;
  if (s.defined.opacityZero && s.opacityZero) mask |= 1u << 28;
  if (s.defined.visibilityHidden && s.visibilityHidden) mask |= 1u << 29;
  if (s.defined.display && (static_cast<unsigned>(s.display) & 2u)) mask |= 1u << 30;
  // Bit 31 announces a byte of values the uint16 below has no room for, written after it: an
  // explicit `inherit` of font-weight (bit 0) and of font-style (bit 1), a font-size relative to
  // the body (bit 2), a font-family (bit 3: its hash follows the multipliers).
  uint8_t extra = 0;
  if (s.defined.fontWeight && s.fontWeight == CssFontWeight::Inherit) extra |= 0x01;
  if (s.defined.fontStyle && s.fontStyle == CssFontStyle::Inherit) extra |= 0x02;
  if (s.defined.fontSizeMultiplier && s.fontSizeRootRelative) extra |= 0x04;
  if (s.defined.fontFamily) extra |= 0x08;
  if (extra != 0) mask |= 1u << 31;
  std::memcpy(p, &mask, 4);
  p += 4;
  uint16_t enums = 0;
  const CssPropertyFlags& d = s.defined;
  if (d.textAlign) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.textAlign) & 7u) << 0);
  if (d.fontStyle) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.fontStyle) & 1u) << 3);
  if (d.fontWeight) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.fontWeight) & 1u) << 4);
  if (d.textDecoration) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.textDecoration) & 3u) << 5);
  if (d.display) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.display) & 1u) << 7);
  if (d.verticalAlign) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.verticalAlign) & 3u) << 8);
  if (d.listStyleNone) enums |= static_cast<uint16_t>((s.listStyleNone ? 1u : 0u) << 10);
  if (d.pageBreakBefore) enums |= static_cast<uint16_t>((s.pageBreakBefore ? 1u : 0u) << 11);
  if (d.pageBreakAfter) enums |= static_cast<uint16_t>((s.pageBreakAfter ? 1u : 0u) << 12);
  if (d.cssFloat) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.cssFloat) & 3u) << 13);
  if (d.smallCaps) enums |= static_cast<uint16_t>((s.smallCaps ? 1u : 0u) << 15);
  std::memcpy(p, &enums, 2);
  p += 2;
  if (extra != 0) *p++ = extra;
  const auto putLen = [&](bool def, const CssLength& l) {
    if (def) {
      std::memcpy(p, &l.value, 4);
      p += 4;
      *p++ = static_cast<uint8_t>(l.unit);
    }
  };
  putLen(d.textIndent, s.textIndent);
  putLen(d.marginTop, s.marginTop);
  putLen(d.marginBottom, s.marginBottom);
  putLen(d.marginLeft, s.marginLeft);
  putLen(d.marginRight, s.marginRight);
  putLen(d.paddingTop, s.paddingTop);
  putLen(d.paddingBottom, s.paddingBottom);
  putLen(d.paddingLeft, s.paddingLeft);
  putLen(d.paddingRight, s.paddingRight);
  putLen(d.imageHeight, s.imageHeight);
  putLen(d.imageWidth, s.imageWidth);
  if (d.lineHeight) {
    std::memcpy(p, &s.lineHeightMultiplier, 4);
    p += 4;
  }
  if (d.fontSizeMultiplier) {
    std::memcpy(p, &s.fontSizeMultiplier, 4);
    p += 4;
  }
  if (d.fontFamily) {
    std::memcpy(p, &s.fontFamily, 4);
    p += 4;
  }
  return static_cast<size_t>(p - out);
}

// Inverse of compressStyle: rebuild a full CssStyle from a compressed record.
static void decompressStyle(const uint8_t* in, CssStyle& out) {
  out.reset();
  const uint8_t* p = in;
  uint32_t mask = 0;
  std::memcpy(&mask, p, 4);
  p += 4;
  uint16_t enums = 0;
  std::memcpy(&enums, p, 2);
  p += 2;
  const uint8_t extra = ((mask >> 31) & 1u) ? *p++ : 0;
  const auto isDefined = [&](int index) { return (mask >> index) & 1u; };
  CssPropertyFlags& d = out.defined;
  d.textAlign = isDefined(0);
  d.fontStyle = isDefined(1);
  d.fontWeight = isDefined(2);
  d.textDecoration = isDefined(3);
  d.textIndent = isDefined(4);
  d.marginTop = isDefined(5);
  d.marginBottom = isDefined(6);
  d.marginLeft = isDefined(7);
  d.marginRight = isDefined(8);
  d.paddingTop = isDefined(9);
  d.paddingBottom = isDefined(10);
  d.paddingLeft = isDefined(11);
  d.paddingRight = isDefined(12);
  d.imageHeight = isDefined(13);
  d.imageWidth = isDefined(14);
  d.display = isDefined(15);
  d.verticalAlign = isDefined(16);
  d.listStyleNone = isDefined(17);
  d.pageBreakBefore = isDefined(18);
  d.pageBreakAfter = isDefined(19);
  d.lineHeight = isDefined(20);
  d.fontSizeMultiplier = isDefined(21);
  d.cssFloat = isDefined(22);
  d.smallCaps = isDefined(23);
  d.colorTransparent = isDefined(24);
  d.opacityZero = isDefined(25);
  d.visibilityHidden = isDefined(26);
  d.fontFamily = (extra & 0x08) != 0 ? 1 : 0;
  if (d.colorTransparent) out.colorTransparent = ((mask >> 27) & 1u) != 0;
  if (d.opacityZero) out.opacityZero = ((mask >> 28) & 1u) != 0;
  if (d.visibilityHidden) out.visibilityHidden = ((mask >> 29) & 1u) != 0;
  if (d.textAlign) out.textAlign = static_cast<CssTextAlign>(enums & 7u);
  if (d.fontStyle) {
    out.fontStyle = (extra & 0x02) ? CssFontStyle::Inherit : static_cast<CssFontStyle>((enums >> 3) & 1u);
  }
  if (d.fontWeight) {
    out.fontWeight = (extra & 0x01) ? CssFontWeight::Inherit : static_cast<CssFontWeight>((enums >> 4) & 1u);
  }
  if (d.textDecoration) out.textDecoration = static_cast<CssTextDecoration>((enums >> 5) & 3u);
  if (d.display) out.display = static_cast<CssDisplay>(((enums >> 7) & 1u) | (((mask >> 30) & 1u) << 1));
  if (d.verticalAlign) out.verticalAlign = static_cast<CssVerticalAlign>((enums >> 8) & 3u);
  if (d.listStyleNone) out.listStyleNone = ((enums >> 10) & 1u) != 0;
  if (d.pageBreakBefore) out.pageBreakBefore = ((enums >> 11) & 1u) != 0;
  if (d.pageBreakAfter) out.pageBreakAfter = ((enums >> 12) & 1u) != 0;
  if (d.cssFloat) out.cssFloat = static_cast<CssFloat>((enums >> 13) & 3u);
  if (d.smallCaps) out.smallCaps = ((enums >> 15) & 1u) != 0;
  const auto getLen = [&](bool def, CssLength& l) {
    if (def) {
      std::memcpy(&l.value, p, 4);
      p += 4;
      l.unit = static_cast<CssUnit>(*p++);
    }
  };
  getLen(d.textIndent, out.textIndent);
  getLen(d.marginTop, out.marginTop);
  getLen(d.marginBottom, out.marginBottom);
  getLen(d.marginLeft, out.marginLeft);
  getLen(d.marginRight, out.marginRight);
  getLen(d.paddingTop, out.paddingTop);
  getLen(d.paddingBottom, out.paddingBottom);
  getLen(d.paddingLeft, out.paddingLeft);
  getLen(d.paddingRight, out.paddingRight);
  getLen(d.imageHeight, out.imageHeight);
  getLen(d.imageWidth, out.imageWidth);
  if (d.lineHeight) {
    std::memcpy(&out.lineHeightMultiplier, p, 4);
    p += 4;
  }
  if (d.fontSizeMultiplier) {
    std::memcpy(&out.fontSizeMultiplier, p, 4);
    p += 4;
    out.fontSizeRootRelative = (extra & 0x04) != 0;
  }
  if (d.fontFamily) std::memcpy(&out.fontFamily, p, 4);
}

// Stream the whole ruleset into the arena as a sorted {hash, styleOff} index plus a pool of
// DISTINCT, sparse-COMPRESSED styles so resolveStyle runs entirely in RAM (FreeInkBook's model,
// adapted, with dedup + compression). One sequential pass over the payload block; each style is
// compressed (compressStyle) and, being canonical, deduplicated by a memcmp against the existing
// length-prefixed pool records. Identical styles (Calibre) collapse to one record, and each is
// far smaller than a flat CssStyle, so a large/repetitive sheet that a flat array couldn't hold
// still fits. Returns false without disturbing arena state when even this won't fit; the caller
// then tries the smaller offset index.
bool CssParser::loadArenaResident(FsFile& file, const uint16_t ruleCount, const uint32_t totalCandidates,
                                  const uint32_t unsupportedSkips) const {
  // Reserve a block so a failed attempt (won't fit, or a stream error) leaves the arena
  // cursor exactly where it was — the caller then reuses that space for the smaller offset
  // index. The file stays open; the caller (ensureCacheIndexLoaded) owns closing it.
  auto block = indexArena_->reserveBlock();
  ResidentEntry* index = ruleCount > 0 ? indexArena_->allocArray<ResidentEntry>(ruleCount) : nullptr;
  if (ruleCount > 0 && index == nullptr) {
    indexArena_->release(block);
    return false;  // even the index won't fit — caller falls back to the offset index
  }

  // Compressed distinct-style pool: contiguous [u8 len][len bytes] records bump-allocated right
  // after the index (only these allocations happen during the loop, so the pool stays contiguous
  // and self-delimiting for the dedup scan). styleOff addresses each record's length prefix.
  uint8_t* poolBase = nullptr;
  size_t poolBytes = 0;
  uint16_t poolCount = 0;

  if (ruleCount > 0) {
    // Skip the sorted offset index (11-byte header + ruleCount * 8) to reach the payload block,
    // then stream it in one sequential pass (fixed-size payloads → no per-record seeking).
    if (!file.seek(static_cast<uint32_t>(CssParser::CSS_CACHE_HEADER_BYTES + complexBytes_ +
                                         static_cast<size_t>(ruleCount) * sizeof(SelectorEntry)))) {
      indexArena_->release(block);
      return false;
    }
    char selectorBuf[MAX_SELECTOR_LENGTH];
    uint8_t rec[kMaxCompressedStyle];
    for (uint16_t i = 0; i < ruleCount; ++i) {
      uint16_t selectorLen = 0;
      uint16_t order = 0;
      if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) || selectorLen > MAX_SELECTOR_LENGTH ||
          file.read(selectorBuf, selectorLen) != selectorLen || file.read(&order, sizeof(order)) != sizeof(order)) {
        indexArena_->release(block);
        return false;
      }
      CssStyle style;
      if (!readCssStylePayload(file, style)) {
        indexArena_->release(block);
        return false;
      }
      const size_t recLen = compressStyle(style, rec);

      // Intern: reuse an identical record if we've already stored one (memcmp over the canonical
      // compressed bytes), else append a new [len][bytes] record to the pool.
      uint32_t off = 0;
      bool found = false;
      for (size_t scan = 0; scan < poolBytes;) {
        const size_t existingLen = poolBase[scan];
        if (existingLen == recLen && std::memcmp(poolBase + scan + 1, rec, recLen) == 0) {
          off = static_cast<uint32_t>(scan);
          found = true;
          break;
        }
        scan += 1 + existingLen;
      }
      if (!found) {
        auto* slot = static_cast<uint8_t*>(indexArena_->alloc(1 + recLen, 1));
        // The pool grew past the arena even after dedup, or past what a 16-bit styleOff addresses
        // (no arena is that large today) — fall back to the index.
        if (slot == nullptr || (poolBase != nullptr && slot - poolBase > UINT16_MAX)) {
          indexArena_->release(block);
          return false;
        }
        if (poolBase == nullptr) poolBase = slot;  // first record anchors the pool
        off = static_cast<uint32_t>(slot - poolBase);
        slot[0] = static_cast<uint8_t>(recLen);
        std::memcpy(slot + 1, rec, recLen);
        poolBytes = static_cast<size_t>(off) + 1 + recLen;
        ++poolCount;
      }
      index[i] =
          ResidentEntry{selectorHash(std::string_view(selectorBuf, selectorLen)), static_cast<uint16_t>(off), order};
    }
    // Sort by hash so lookupRules can binary-search, matching the on-disk index ordering.
    std::sort(index, index + ruleCount, [](const ResidentEntry& a, const ResidentEntry& b) { return a.hash < b.hash; });
    arenaResident_ = index;
    arenaStylePool_ = poolBase;
    arenaStyleCount_ = poolCount;
    arenaPoolBytes_ = static_cast<uint32_t>(poolBytes);
  }

  indexArena_->commit(block);
  cachedRuleCount_ = ruleCount;
  totalSelectorCandidates_ = totalCandidates;
  unsupportedSelectorSkips_ = unsupportedSkips;
  cacheIndexLoaded_ = true;
  LOG_DBG("CSS", "Loaded CSS RESIDENT in arena: %u selectors -> %u distinct styles, %u index + %u pool bytes",
          static_cast<unsigned>(ruleCount), static_cast<unsigned>(poolCount),
          static_cast<unsigned>(static_cast<size_t>(ruleCount) * sizeof(ResidentEntry)),
          static_cast<unsigned>(poolBytes));
  return true;
}

// Forget the current ruleset view — heap vector or either arena layout — and reset the
// loaded flag so ensureCacheIndexLoaded() reloads. Arena memory is owned by the arena
// (reset per build), so we only drop the non-owning pointers here.
void CssParser::dropIndex() const {
  // swap, not clear(): clear() keeps the capacity, and the index is up to MAX_RULES x 8 B.
  std::vector<SelectorEntry>().swap(cacheRuleOffsets_);
  arenaResident_ = nullptr;
  arenaStylePool_ = nullptr;
  arenaStyleCount_ = 0;
  arenaPoolBytes_ = 0;
  arenaIndex_ = nullptr;
  complexHeap_.reset();
  complexArena_ = nullptr;
  complexBytes_ = 0;
  complexCapacity_ = 0;
  cacheIndexLoaded_ = false;
  cachedRuleCount_ = 0;
}

CssParser::ResidentFootprint CssParser::getResidentFootprint() const {
  ResidentFootprint f;
  if (arenaResident_ == nullptr) return f;  // only the RESIDENT layout has a pooled footprint
  f.ruleCount = static_cast<uint16_t>(cachedRuleCount_);
  f.distinctStyles = arenaStyleCount_;
  f.indexBytes = static_cast<uint32_t>(cachedRuleCount_) * sizeof(ResidentEntry);
  f.poolBytes = arenaPoolBytes_;
  return f;
}

// --- Complex rules (see CssParser.h, complexTable) ---

bool CssParser::growComplexTable(const size_t needed) {
  if (needed <= complexCapacity_) return true;
  if (needed > MAX_COMPLEX_BYTES) return false;
  const size_t capacity =
      std::min(MAX_COMPLEX_BYTES, std::max<size_t>(needed, complexCapacity_ ? complexCapacity_ * 2u : 256u));
  auto grown = makeUniqueNoThrow<uint8_t[]>(capacity);
  if (!grown) {
    LOG_ERR("CSS", "No heap for %u bytes of complex CSS rules", static_cast<unsigned>(capacity));
    return false;
  }
  if (complexHeap_) std::memcpy(grown.get(), complexHeap_.get(), complexBytes_);
  complexHeap_ = std::move(grown);
  complexCapacity_ = static_cast<uint16_t>(capacity);
  return true;
}

bool CssParser::addComplexRule(const std::string_view selector, const CssStyle& style, const uint16_t order) {
  // The compounds right to left, the subject first, each step marked when it must be the parent
  // of the step before it (`div > p`) rather than any ancestor (`div p`).
  Compound compounds[MAX_COMPLEX_STEPS];
  uint8_t steps[MAX_COMPLEX_STEPS] = {};
  size_t count = 0;
  uint32_t specificity = 0;
  for (std::string_view rest = selector;;) {
    size_t start = rest.size();
    while (start > 0 && rest[start - 1] != ' ' && rest[start - 1] != '>') --start;
    if (count == MAX_COMPLEX_STEPS || !parseCompound(rest.substr(start), compounds[count])) return false;
    specificity = addSpecificity(specificity, compounds[count].specificity());
    rest = rest.substr(0, start);
    ++count;
    if (rest.empty()) break;
    int children = 0;
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '>')) {
      children += rest.back() == '>' ? 1 : 0;
      rest.remove_suffix(1);
    }
    if (rest.empty() || children > 1 || count == MAX_COMPLEX_STEPS) return false;
    if (children == 1) steps[count] = STEP_CHILD;
  }
  uint8_t styleBytes[kMaxCompressedStyle];
  const size_t styleLen = compressStyle(style, styleBytes);

  if (complexBytes_ == 0) {
    if (!growComplexTable(COMPLEX_HEADER_BYTES)) return false;
    std::memset(complexHeap_.get(), 0, COMPLEX_HEADER_BYTES);
    complexBytes_ = COMPLEX_HEADER_BYTES;
  }
  if (complexHeap_.get()[1] >= MAX_COMPLEX_RULES) return false;

  // Each compound's index, interning it when the table does not have it yet. A cap reached part
  // way leaves the compounds interned so far unreferenced, which costs their bytes and nothing else.
  const auto intern = [this](const Compound& compound) -> int {
    uint8_t bytes[MAX_COMPOUND_BYTES];
    const size_t len = compound.serialize(bytes);
    const size_t compoundsEnd = COMPLEX_HEADER_BYTES + readU16(complexHeap_.get() + 2);
    for (size_t pos = COMPLEX_HEADER_BYTES, i = 0; i < complexHeap_.get()[0]; ++i) {
      const size_t existing = compoundSize(complexHeap_.get()[pos]);
      if (existing == len && std::memcmp(complexHeap_.get() + pos, bytes, len) == 0) return static_cast<int>(i);
      pos += existing;
    }
    if (complexHeap_.get()[0] >= MAX_COMPLEX_COMPOUNDS || !growComplexTable(complexBytes_ + len)) return -1;
    uint8_t* table = complexHeap_.get();
    std::memmove(table + compoundsEnd + len, table + compoundsEnd, complexBytes_ - compoundsEnd);
    std::memcpy(table + compoundsEnd, bytes, len);
    const auto compoundBytes = static_cast<uint16_t>(compoundsEnd + len - COMPLEX_HEADER_BYTES);
    std::memcpy(table + 2, &compoundBytes, sizeof(compoundBytes));
    complexBytes_ = static_cast<uint16_t>(complexBytes_ + len);
    return table[0]++;
  };
  for (size_t i = 0; i < count; ++i) {
    const int index = intern(compounds[i]);
    if (index < 0) return false;
    steps[i] = static_cast<uint8_t>(steps[i] | index);
  }

  const size_t ruleLen = COMPLEX_RULE_HEADER_BYTES + count + styleLen;
  if (!growComplexTable(complexBytes_ + ruleLen)) return false;
  uint8_t* table = complexHeap_.get();
  uint8_t* rule = table + complexBytes_;
  const auto packed = static_cast<uint16_t>(specificity);
  std::memcpy(rule, &order, sizeof(order));
  std::memcpy(rule + 2, &packed, sizeof(packed));
  rule[4] = static_cast<uint8_t>(count);
  rule[5] = static_cast<uint8_t>(styleLen);
  std::memcpy(rule + COMPLEX_RULE_HEADER_BYTES, steps, count);
  std::memcpy(rule + COMPLEX_RULE_HEADER_BYTES + count, styleBytes, styleLen);
  complexBytes_ = static_cast<uint16_t>(complexBytes_ + ruleLen);
  ++table[1];
  // A compound with an id anywhere in the selector: the element's own id may matter, or an
  // ancestor's -- either way the caller must not share one style between elements by tag|class.
  for (size_t i = 0; i < count; ++i) {
    if (compounds[i].flags & COMPOUND_HAS_ID) hasIdSelectors_ = true;
  }
  return true;
}

uint64_t CssParser::matchCompounds(const uint32_t tagHash, const uint32_t* idHash, const uint32_t* classHashes,
                                   const size_t classCount) const {
  const uint8_t* table = complexTable();
  if (table == nullptr) return 0;
  // Which compounds the element matches: every part a compound names must be the element's.
  uint64_t matched = 0;
  const uint8_t* p = table + COMPLEX_HEADER_BYTES;
  for (uint8_t i = 0; i < table[0]; ++i) {
    const uint8_t flags = *p++;
    bool match = true;
    uint32_t name = 0;
    if (flags & COMPOUND_HAS_TAG) {
      std::memcpy(&name, p, 4);
      p += 4;
      match = name == tagHash;
    }
    if (flags & COMPOUND_HAS_ID) {
      std::memcpy(&name, p, 4);
      p += 4;
      match = match && idHash != nullptr && name == *idHash;
    }
    for (uint8_t c = 0; c < (flags & COMPOUND_CLASS_COUNT); ++c) {
      std::memcpy(&name, p, 4);
      p += 4;
      match = match && std::find(classHashes, classHashes + classCount, name) != classHashes + classCount;
    }
    if (match) matched |= uint64_t{1} << i;
  }
  return matched;
}

uint64_t CssParser::compoundMask(const std::string& tagName, const std::string& classAttr,
                                 const std::string& idAttr) const {
  if (complexBytes_ == 0) return 0;
  uint32_t classHashes[MAX_ELEMENT_CLASSES];
  size_t classCount = 0;
  std::string token;
  forEachNormalizedClassToken(classAttr, token, [&](const std::string& cls) {
    if (classCount < MAX_ELEMENT_CLASSES) classHashes[classCount++] = selectorHash(cls);
  });
  const uint32_t idHash = fnv1a32Lower(idAttr);
  return matchCompounds(fnv1a32Lower(tagName), idAttr.empty() ? nullptr : &idHash, classHashes, classCount);
}

void CssParser::applyComplexRules(const uint64_t self, const Ancestry* ancestry, Cascade& cascade,
                                  bool* dependsOnAncestors) const {
  const uint8_t* table = complexTable();
  if (table == nullptr || self == 0) return;
  const uint8_t* p = table + COMPLEX_HEADER_BYTES + readU16(table + 2);
  for (uint8_t r = 0; r < table[1]; ++r) {
    const uint8_t steps = p[4];
    const uint8_t styleLen = p[5];
    const uint8_t* chain = p + COMPLEX_RULE_HEADER_BYTES;
    if ((self >> (chain[0] & STEP_COMPOUND)) & 1u) {
      // A rule with ancestor steps makes the result depend on where the element stands, matched
      // or not: the caller must not reuse it for another element with the same tag and class.
      if (steps > 1 && dependsOnAncestors != nullptr) *dependsOnAncestors = true;
      if (steps == 1 || (ancestry != nullptr && ancestry->masks != nullptr &&
                         chainMatches(chain, steps, ancestry->masks, ancestry->depth))) {
        CssStyle style;
        decompressStyle(chain + steps, style);
        cascade.apply(style, readU16(p + 2), readU16(p));
      }
    }
    p += COMPLEX_RULE_HEADER_BYTES + steps + styleLen;
  }
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr, const std::string& idAttr,
                                 const Ancestry* ancestry, bool* dependsOnAncestors) const {
  if (dependsOnAncestors != nullptr) *dependsOnAncestors = false;
  static bool lowHeapWarningLogged = false;
  resolveStats_.resolveCalls++;
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Lean/arena builds keep the resolver's footprint off the heap (index in arena, hot cache
  // disabled), so they stay correct at the lower LEAN_MIN_FREE_HEAP_FOR_CSS floor. RESIDENT
  // lookups ignore lowHeapMode entirely inside lookupRules — they touch no SD.
  const bool lowHeapMode = freeHeap < (leanResolve_ ? LEAN_MIN_FREE_HEAP_FOR_CSS : MIN_FREE_HEAP_FOR_CSS);
  if (lowHeapMode) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      // Report the floor that ACTUALLY applied, and whether the skip can cost anything. The
      // old message hardcoded MIN_FREE_HEAP_FOR_CSS even in lean mode, so a 960-byte dip under
      // the 24 KB lean floor read as "17 KB below 40960" — which misdiagnosed this exact bug
      // once already. Resident rulesets serve every lookup from the arena, so a skip there is
      // harmless: say so rather than warning about missing styles that cannot happen.
      LOG_DBG("CSS", "Low heap (%u bytes) below %s floor (%u), skipping disk CSS lookups (%s)", freeHeap,
              leanResolve_ ? "LEAN" : "full",
              static_cast<unsigned>(leanResolve_ ? LEAN_MIN_FREE_HEAP_FOR_CSS : MIN_FREE_HEAP_FOR_CSS),
              arenaResident_ ? "harmless: ruleset is arena-resident" : "styles may be missing");
    }
    resolveStats_.lowHeapSkips++;
  }
  // Every matching rule is merged in ranked by specificity, then stylesheet order, so the
  // lookups below may run in any order (Cascade, CssStyle::applyOver). The fixed sequence they
  // used to be applied in interleaved .a and p.a per class, which let a plain `.b` beat `p.a`
  // and made the class attribute's order, not the stylesheet's, settle ties between classes.
  // Based on crosspoint-reader PR #3891 (Justin Mitchell, @itsthisjustin), which sorts the matches
  // by specificity and source order; reworked to rank each property instead, so no match list
  // (and no style per match) has to be held while the index is read from disk.
  Cascade cascade;
  const auto lookup = [&](const std::string& selector, const uint32_t specificity) {
    if (lookupRules(selector, specificity, cascade, !lowHeapMode) && lowHeapMode) {
      resolveStats_.lowHeapRescuedHits++;
    }
  };
  const std::string tag = normalized(tagName);

  lookup(tag, SPECIFICITY_TAG);

  // The element's class names, hashed for the complex rules when the sheet has any and the caller
  // has not matched the element against them already (Ancestry::self).
  const bool complexRules = complexBytes_ > 0;
  const bool hashClasses = complexRules && ancestry == nullptr;
  uint32_t classHashes[MAX_ELEMENT_CLASSES];
  size_t classCount = 0;

  if (!classAttr.empty()) {
    std::string classToken;
    std::string classKey;
    classKey.reserve(32);
    std::string combinedKey;
    combinedKey.reserve(tag.size() + 1 + 32);

    forEachNormalizedClassToken(classAttr, classToken, [&](const std::string& cls) {
      classKey.clear();
      classKey.push_back('.');
      classKey.append(cls);
      lookup(classKey, SPECIFICITY_CLASS);

      combinedKey.clear();
      combinedKey.append(tag);
      combinedKey.push_back('.');
      combinedKey.append(cls);
      lookup(combinedKey, SPECIFICITY_CLASS + SPECIFICITY_TAG);

      if (hashClasses && classCount < MAX_ELEMENT_CLASSES) classHashes[classCount++] = selectorHash(cls);
    });
  }

  // Skipped outright when the sheet has no id selector at all, which is the common case: both
  // lookups below would build a key, hash it, probe the index and come back empty, twice per
  // id-bearing element. On a converted endnotes chapter with one generated id per note that was
  // ~700 guaranteed misses per chapter.
  if (!idAttr.empty() && hasIdSelectors_) {
    std::string idKey;
    idKey.reserve(1 + idAttr.size());
    idKey.push_back('#');
    for (const char c : idAttr) {
      idKey.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    lookup(idKey, SPECIFICITY_ID);

    std::string tagIdKey;
    tagIdKey.reserve(tag.size() + idKey.size());
    tagIdKey.append(tag);
    tagIdKey.append(idKey);
    lookup(tagIdKey, SPECIFICITY_ID + SPECIFICITY_TAG);
  }

  if (complexRules) {
    const uint32_t idHash = fnv1a32Lower(idAttr);
    const uint64_t self = ancestry != nullptr ? ancestry->self
                                              : matchCompounds(selectorHash(tag), idAttr.empty() ? nullptr : &idHash,
                                                               classHashes, classCount);
    applyComplexRules(self, ancestry, cascade, dependsOnAncestors);
  }

  if (!cascade.result.defined.anySet()) {
    resolveStats_.misses++;
  }

  return cascade.result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  const std::string backgrounds = cachePath + backgroundImagesFile;
  if (Storage.exists(backgrounds.c_str())) Storage.remove(backgrounds.c_str());
  const std::string fonts = cachePath + fontFacesFile;
  if (Storage.exists(fonts.c_str())) Storage.remove(fonts.c_str());
}

CssParser::BackgroundDeclaration CssParser::parseBackground(const std::string_view declarations) {
  BackgroundDeclaration result;

  std::string_view name;
  std::string_view value;
  for (std::string_view rest = declarations; nextDeclaration(rest, name, value);) {
    const bool image = equalsIgnoreCase(name, "background-image");
    const bool shorthand = equalsIgnoreCase(name, "background");
    if (equalsIgnoreCase(name, "background-repeat") || shorthand) {
      if (findIgnoreCase(value, "no-repeat") != std::string_view::npos) result.noRepeat = true;
    }
    if (!image && !shorthand) continue;
    // A colour, `none` or a gradient replaces the picture as much as a new url does.
    result.declared = true;
    result.url.clear();
    const size_t open = findIgnoreCase(value, "url(");
    if (open == std::string_view::npos) continue;
    const size_t close = value.find(')', open + 4);
    if (close == std::string_view::npos) continue;
    std::string_view url = trimCss(value.substr(open + 4, close - open - 4));
    if (url.size() >= 2 && (url.front() == '"' || url.front() == '\'') && url.back() == url.front()) {
      url = url.substr(1, url.size() - 2);
    }
    // Inline data and remote pictures are not in the book.
    if (url.empty() || findIgnoreCase(url, "data:") == 0 || url.find("://") != std::string_view::npos) continue;
    while (!url.empty() && url.front() == '/') url.remove_prefix(1);
    result.url.assign(url);
  }
  return result;
}

// --- @font-face ---
// Based on crosspoint-reader PR #3934 (Justin Mitchell, @itsthisjustin): parseFontFace and the
// face list's side file. Reworked for this parser: the name and weight are kept for the book font
// extraction, and the src entry is ranked rather than taken first-usable.

uint32_t CssParser::fontFamilyHash(const std::string_view name) {
  // FNV-1a over the name lowercased, quotes and outer whitespace gone and inner runs of whitespace
  // as one space: what normalizedInto() leaves of a font-family value.
  const std::string_view v = unquoteCss(name);
  uint32_t h = 2166136261u;
  bool space = false;
  for (const char c : v) {
    if (isCssWhitespace(c)) {
      space = true;
      continue;
    }
    if (space) {
      h = (h ^ static_cast<uint8_t>(' ')) * 16777619u;
      space = false;
    }
    h = (h ^ static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)))) * 16777619u;
  }
  h = CssFontFamily::identity(h);  // the low bits carry a generic family (CssFontFamily)
  return h != 0 ? h : CssFontFamily::kGenericMask + 1;
}

void CssParser::parseFontFace(const std::string_view declarations) {
  std::string_view family;
  std::string_view src;
  uint16_t weight = 400;
  bool italic = false;
  std::string_view prop;
  std::string_view value;
  for (std::string_view rest = declarations; nextDeclaration(rest, prop, value);) {
    value = stripTrailingImportant(value);
    if (equalsIgnoreCase(prop, "font-family")) {
      family = unquoteCss(value);
    } else if (equalsIgnoreCase(prop, "src")) {
      src = value;
    } else if (equalsIgnoreCase(prop, "font-style")) {
      italic = !equalsIgnoreCase(value, "normal");
    } else if (equalsIgnoreCase(prop, "font-weight")) {
      uint16_t number = 0;  // a number up to 999, else 0
      for (const char c : value.substr(0, 3)) number = c >= '0' && c <= '9' ? number * 10 + (c - '0') : 0;
      weight = findIgnoreCase(value, "bold") != std::string_view::npos ? 700 : number != 0 ? number : 400;
    }
  }
  if (family.empty() || family.size() > MAX_FONT_FACE_TEXT || fontFaceCount_ >= MAX_FONT_FACES) return;

  // src: a comma-separated list of url(...) [format(...)] and local(...). Rank each url by what
  // the reader can draw: 0 TrueType, 1 OpenType (TrueType or CFF outlines), 2 anything else; WOFF2,
  // SVG and EOT never, nor fonts that are not in the book. The first kind whose extension or
  // format() matches decides, the refused kinds first.
  struct Kind {
    const char* ext;
    const char* format;
    int rank;  // -1: refused
  };
  static constexpr Kind KINDS[] = {
      {"woff2", "woff2", -1}, {"svg", "svg", -1},     {"eot", "embedded-opentype", -1},
      {"ttf", "truetype", 0}, {"otf", "opentype", 1},
  };
  int bestRank = 3;
  std::string_view best;
  for (size_t at = 0; (at = findIgnoreCase(src, "url(")) != std::string_view::npos;) {
    const size_t close = src.find(')', at);
    if (close == std::string_view::npos) break;
    std::string_view url = unquoteCss(src.substr(at + 4, close - at - 4));
    url = url.substr(0, url.find_first_of("?#"));  // `font.eot?#iefix`: not part of the path
    src.remove_prefix(close);
    const std::string_view format = src.substr(0, src.find(','));  // ") format('woff2')"
    const size_t dot = url.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == url.size() || findIgnoreCase(url, "data:") == 0 ||
        url.find("://") != std::string_view::npos) {
      continue;
    }
    const std::string_view ext = url.substr(dot + 1);
    int rank = 2;
    for (const Kind& kind : KINDS) {
      if (equalsIgnoreCase(ext, kind.ext) || findIgnoreCase(format, kind.format) != std::string_view::npos) {
        rank = kind.rank;
        break;
      }
    }
    if (rank >= 0 && rank < bestRank) {
      bestRank = rank;
      best = url;
    }
  }
  const size_t hrefLen = stylesheetDir_.size() + best.size();
  if (best.empty() || hrefLen > MAX_FONT_FACE_TEXT) return;
  const char head[] = {static_cast<char>(weight), static_cast<char>(weight >> 8), static_cast<char>(italic),
                       static_cast<char>(family.size())};
  fontFaces_.append(head, sizeof(head));
  fontFaces_.append(family.data(), family.size());
  fontFaces_ += static_cast<char>(hrefLen);
  fontFaces_ += stylesheetDir_;
  fontFaces_.append(best.data(), best.size());
  ++fontFaceCount_;
}

void CssParser::saveFontFaces() const {
  const std::string file = cachePath + fontFacesFile;
  if (fontFaces_.empty()) {
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    return;
  }
  FsFile out;
  if (!Storage.openFileForWrite("CSS", file, out)) return;
  out.write(CSS_CACHE_VERSION);
  out.write(reinterpret_cast<const uint8_t*>(fontFaces_.data()), fontFaces_.size());
  out.close();
}

bool CssParser::loadFontFaces(const std::string& cachePath, std::string& records) {
  const std::string file = cachePath + fontFacesFile;
  constexpr size_t MAX_FILE = 1 + MAX_FONT_FACES * (5 + 2 * MAX_FONT_FACE_TEXT);
  // Checked first: the file is absent for almost every book, and a failed open logs.
  if (!Storage.exists(file.c_str()) || !Storage.readFileToString("CSS", file, MAX_FILE, records) ||
      records[0] != static_cast<char>(CSS_CACHE_VERSION)) {
    records.clear();
    return false;
  }
  records.erase(0, 1);
  return !records.empty();
}

bool CssParser::nextFontFace(std::string_view& records, FontFace& face) {
  const auto* p = reinterpret_cast<const uint8_t*>(records.data());
  // weight(2) italic(1) nameLen(1) name hrefLen(1) href
  const size_t hrefAt = records.size() >= 5 ? 5 + p[3] : records.size() + 1;
  if (records.size() < hrefAt || records.size() < hrefAt + p[hrefAt - 1]) return false;
  face.weight = static_cast<uint16_t>(p[0] | p[1] << 8);
  face.italic = p[2] != 0;
  face.name = std::string_view(records.data() + 4, p[3]);
  face.href = std::string_view(records.data() + hrefAt, p[hrefAt - 1]);
  face.family = fontFamilyHash(face.name);
  records.remove_prefix(hrefAt + face.href.size());
  return true;
}

void CssParser::setStylesheetPath(const std::string& sheetPathInEpub) {
  const size_t slash = sheetPathInEpub.rfind('/');
  stylesheetDir_ = slash == std::string::npos ? std::string() : sheetPathInEpub.substr(0, slash + 1);
}

void CssParser::recordBackgroundImage(const std::string_view selectorGroup, const std::string& path) {
  std::string key;
  size_t partStart = 0;
  for (size_t i = 0; i <= selectorGroup.size(); ++i) {
    if (i != selectorGroup.size() && selectorGroup[i] != ',') continue;
    normalizedInto(selectorGroup.substr(partStart, i - partStart), key);
    partStart = i + 1;
    if (key.empty() || key.size() > MAX_SELECTOR_LENGTH || !isSelectorUsableByResolver(key)) continue;
    auto existing = std::find_if(backgroundImages_.begin(), backgroundImages_.end(),
                                 [&](const BackgroundImageRule& r) { return r.selector == key; });
    if (existing != backgroundImages_.end()) {
      // The later rule wins, a `none` included, and takes the later place: the list is kept in
      // stylesheet order, which settles ties between selectors in backgroundImageFor.
      BackgroundImageRule redeclared{std::move(existing->selector), path};
      backgroundImages_.erase(existing);
      backgroundImages_.push_back(std::move(redeclared));
    } else if (!path.empty() && backgroundImages_.size() < MAX_BACKGROUND_IMAGE_RULES) {
      backgroundImages_.push_back({key, path});
    }
  }
}

void CssParser::saveBackgroundImages() const {
  const std::string file = cachePath + backgroundImagesFile;
  uint16_t count = 0;
  for (const auto& r : backgroundImages_) count += r.path.empty() ? 0 : 1;
  if (count == 0) {
    if (Storage.exists(file.c_str())) Storage.remove(file.c_str());
    return;
  }
  FsFile out;
  if (!Storage.openFileForWrite("CSS", file, out)) return;
  out.write(BACKGROUND_IMAGES_FILE_VERSION);
  out.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
  for (const auto& r : backgroundImages_) {
    if (r.path.empty()) continue;
    for (const std::string* field : {&r.selector, &r.path}) {
      const auto len = static_cast<uint16_t>(field->size());
      out.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len));
      out.write(reinterpret_cast<const uint8_t*>(field->data()), len);
    }
  }
  out.close();
  LOG_DBG("CSS", "Recorded %u background picture rule(s)", count);
}

void CssParser::loadBackgroundImages() const {
  backgroundImagesLoaded_ = true;
  backgroundImages_.clear();
  const std::string file = cachePath + backgroundImagesFile;
  // Checked first: the file is absent for almost every book, and a failed open logs.
  if (cachePath.empty() || !Storage.exists(file.c_str())) return;
  FsFile in;
  if (!Storage.openFileForRead("CSS", file, in)) return;
  uint8_t version = 0;
  uint16_t count = 0;
  if (in.read(&version, 1) != 1 || version != BACKGROUND_IMAGES_FILE_VERSION ||
      in.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) {
    return;
  }
  count = std::min<uint16_t>(count, MAX_BACKGROUND_IMAGE_RULES);
  backgroundImages_.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    BackgroundImageRule rule;
    for (std::string* field : {&rule.selector, &rule.path}) {
      uint16_t len = 0;
      if (in.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len) || len > 512) {
        backgroundImages_.clear();
        return;
      }
      field->resize(len);
      if (len > 0 && in.read(reinterpret_cast<uint8_t*>(&(*field)[0]), len) != len) {
        backgroundImages_.clear();
        return;
      }
    }
    backgroundImages_.push_back(std::move(rule));
  }
}

const std::string* CssParser::backgroundImageFor(const std::string& tagName, const std::string& classAttr,
                                                 const std::string& idAttr) const {
  if (!backgroundImagesLoaded_) loadBackgroundImages();
  if (backgroundImages_.empty()) return nullptr;
  // resolveStyle's cascade: the most specific matching rule wins, and between equally specific
  // ones the later in the stylesheets (the list is in that order, see recordBackgroundImage).
  const BackgroundImageRule* result = nullptr;
  uint32_t resultSpecificity = 0;
  const auto consider = [&](const std::string& key, const uint32_t specificity) {
    for (const auto& r : backgroundImages_) {
      if (r.selector != key) continue;
      if (!result || specificity > resultSpecificity || (specificity == resultSpecificity && &r > result)) {
        result = &r;
        resultSpecificity = specificity;
      }
      return;
    }
  };
  const std::string tag = normalized(tagName);
  consider(tag, SPECIFICITY_TAG);
  if (!classAttr.empty()) {
    std::string token;
    std::string key;
    forEachNormalizedClassToken(classAttr, token, [&](const std::string& cls) {
      key = "." + cls;
      consider(key, SPECIFICITY_CLASS);
      key = tag + "." + cls;
      consider(key, SPECIFICITY_CLASS + SPECIFICITY_TAG);
    });
  }
  if (!idAttr.empty()) {
    std::string id = "#";
    for (const char c : idAttr) id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    consider(id, SPECIFICITY_ID);
    consider(tag + id, SPECIFICITY_ID + SPECIFICITY_TAG);
  }
  return (result && !result->path.empty()) ? &result->path : nullptr;
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  file.write(CssParser::CSS_CACHE_VERSION);
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));
  file.write(reinterpret_cast<const uint8_t*>(&totalSelectorCandidates_), sizeof(totalSelectorCandidates_));
  file.write(reinterpret_cast<const uint8_t*>(&unsupportedSelectorSkips_), sizeof(unsupportedSelectorSkips_));
  file.write(static_cast<uint8_t>((hasIdSelectors_ ? 1 : 0) | (rulesTruncated_ ? 2 : 0)));  // v17 flags byte
  writeComplexTable(file);
  // v10: index lives immediately after the header (before rule payloads).
  // Write zeroed placeholder entries now; patch with sorted data below.
  // ensureCacheIndexLoaded() reads header + index sequentially — no seek over payloads.
  std::vector<SelectorEntry> indexEntries;
  indexEntries.reserve(ruleCount);
  {
    constexpr SelectorEntry kZero{};
    for (uint16_t i = 0; i < ruleCount; ++i) {
      file.write(reinterpret_cast<const uint8_t*>(&kZero), sizeof(kZero));
    }
  }

  // Write each rule: selector string (length-prefixed) + style payload.
  // Record {hash, ruleOffset} pairs as we go so we can build the sorted index.
  for (const auto& pair : rulesBySelector_) {
    const uint32_t ruleOffset = file.position();
    writeRuleRecord(file, pair.first, pair.second.order, pair.second.style);
    indexEntries.push_back({selectorHash(pair.first), ruleOffset});
  }

  // Sort and patch the index placeholder at position 11.
  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  file.seek(CssParser::CSS_CACHE_HEADER_BYTES + complexBytes_);
  for (const auto& entry : indexEntries) {
    file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  file.close();
  saveBackgroundImages();
  saveFontFaces();
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  // Drop parse-time in-memory rules and LRU caches.
  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  // Arena mode: any view from a previous build points into an arena that has since been reset,
  // so force a fresh load into the current arena. Heap mode: if clearCaches() retained the disk
  // index, ensureCacheIndexLoaded() short-circuits — don't evict it here.
  if (indexArena_) {
    dropIndex();
  } else if (!cacheIndexLoaded_) {
    cacheRuleOffsets_.clear();
    cachedRuleCount_ = 0;
  }

  if (!ensureCacheIndexLoaded()) {
    return false;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache index", static_cast<unsigned>(cachedRuleCount_));
  return true;
}
