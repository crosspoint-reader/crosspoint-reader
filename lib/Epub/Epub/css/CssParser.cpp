#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  bool push_back(char c) {
    if (len >= CAPACITY) return false;
    data[len++] = c;
    return true;
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Flat rule-store caps. The index is 12KB at MAX_RULES, selector text is
// bounded to 32KB, and deduplicated style bodies are bounded to about 26KB.
constexpr size_t MAX_RULES = 1500;
constexpr size_t SELECTOR_POOL_CAP = 32 * 1024;
constexpr size_t MAX_UNIQUE_STYLES = 256;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Separates a stored key's subject compound from its ancestor part. It sorts
// below every selector character, so all contextual rules for one subject are
// contiguous directly after the subject's own entry.
constexpr std::string_view CONTEXT_SEPARATOR{"\x1f", 1};
constexpr std::string_view ID_MARKER = "#";
constexpr std::string_view CLASS_MARKER = ".";
constexpr std::string_view FIRST_LETTER_MARKER = "::first-letter";
// A stored key is the (reordered) selector plus the context separator.
constexpr size_t MAX_STORED_KEY_LENGTH = MAX_SELECTOR_LENGTH + 1;

// Element classes considered per lookup; compound-class subsets use only the first few.
constexpr size_t MAX_ELEMENT_CLASSES = 8;
constexpr size_t MAX_COMPOUND_SUBSET_CLASSES = 4;
constexpr size_t MAX_STYLE_MATCHES = 16;

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

constexpr bool isIdentChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool lessIgnoringAsciiCase(const std::string_view a, const std::string_view b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](const char x, const char y) {
    return static_cast<unsigned char>(asciiToLower(x)) < static_cast<unsigned char>(asciiToLower(y));
  });
}

void sortIgnoringAsciiCase(std::string_view* items, const size_t count) {
  std::sort(items, items + count, lessIgnoringAsciiCase);
}

// FNV-1a over lowercase ASCII. Never 0, so 0 can mean "absent".
uint32_t hashIgnoringAsciiCase(const std::string_view s) {
  uint32_t hash = 2166136261u;
  for (const char c : s) {
    hash ^= static_cast<unsigned char>(asciiToLower(c));
    hash *= 16777619u;
  }
  return hash ? hash : 1;
}

// One compound selector: optional element (empty = any), optional #id, classes.
struct CompoundSelector {
  std::string_view tag;
  std::string_view id;
  std::string_view classes[CssAncestor::MAX_CLASSES];
  uint8_t classCount = 0;

  [[nodiscard]] bool empty() const { return tag.empty() && id.empty() && classCount == 0; }
};

// Parses "p", "*", ".a.b", "p#id.a". Rejects everything else.
bool parseCompound(const std::string_view text, CompoundSelector& out) {
  out = CompoundSelector{};
  if (text.empty()) return false;
  size_t i = 0;
  if (text[0] == '*') {
    i = 1;
  } else {
    while (i < text.size() && isIdentChar(text[i])) ++i;
    out.tag = text.substr(0, i);
  }
  while (i < text.size()) {
    const char marker = text[i++];
    const size_t start = i;
    while (i < text.size() && isIdentChar(text[i])) ++i;
    const std::string_view name = text.substr(start, i - start);
    if (name.empty()) return false;
    if (marker == '#' && out.id.empty()) {
      out.id = name;
    } else if (marker == '.' && out.classCount < CssAncestor::MAX_CLASSES) {
      out.classes[out.classCount++] = name;
    } else {
      return false;
    }
  }
  return true;
}

// Splits the trailing compound off `text`, leaving what precedes it (including the combinator).
std::string_view popTrailingCompound(std::string_view& text) {
  size_t start = text.size();
  while (start > 0 && !isCssWhitespace(text[start - 1]) && text[start - 1] != '>') --start;
  const std::string_view compound = text.substr(start);
  text = text.substr(0, start);
  return compound;
}

// Strips a trailing combinator; returns true when it was a child combinator ('>').
bool popTrailingCombinator(std::string_view& text) {
  bool child = false;
  while (!text.empty() && (isCssWhitespace(text.back()) || text.back() == '>')) {
    child = child || text.back() == '>';
    text.remove_suffix(1);
  }
  return child;
}

bool compoundMatches(const CompoundSelector& compound, const CssAncestor& ancestor) {
  if (!compound.tag.empty() && hashIgnoringAsciiCase(compound.tag) != ancestor.tagHash) return false;
  if (!compound.id.empty() && hashIgnoringAsciiCase(compound.id) != ancestor.idHash) return false;
  for (uint8_t i = 0; i < compound.classCount; ++i) {
    const uint32_t hash = hashIgnoringAsciiCase(compound.classes[i]);
    const uint32_t* end = ancestor.classHashes + ancestor.classCount;
    if (std::find(ancestor.classHashes, end, hash) == end) return false;
  }
  return true;
}

// Matches a stored ancestor part ("div.poem > ", "body div ") right to left
// against open ancestors (outermost first). Descendant steps bind to the nearest
// matching ancestor.
bool ancestorsMatch(std::string_view context, const CssAncestor* ancestors, size_t candidates) {
  while (true) {
    const bool child = popTrailingCombinator(context);
    if (context.empty()) return true;
    CompoundSelector compound;
    if (!parseCompound(popTrailingCompound(context), compound)) return false;
    if (child) {
      if (candidates == 0 || !compoundMatches(compound, ancestors[candidates - 1])) return false;
    } else {
      while (candidates > 0 && !compoundMatches(compound, ancestors[candidates - 1])) --candidates;
      if (candidates == 0) return false;
    }
    --candidates;
  }
}

// Specificity packed as ids << 16 | classes << 8 | elements.
constexpr uint32_t packSpecificity(const uint32_t ids, const uint32_t classes, const uint32_t elements) {
  return ids << 16 | classes << 8 | elements;
}

uint32_t contextSpecificity(std::string_view context) {
  uint32_t specificity = 0;
  while (true) {
    popTrailingCombinator(context);
    if (context.empty()) return specificity;
    CompoundSelector compound;
    if (!parseCompound(popTrailingCompound(context), compound)) return specificity;
    specificity += packSpecificity(compound.id.empty() ? 0 : 1, compound.classCount, compound.tag.empty() ? 0 : 1);
  }
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

// Parse the entirety of s as a number into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

// True for the (page-)break-before/after values that force a new page.
bool interpretForcedBreak(const std::string_view value) {
  return iequalsAscii(value, "always") || iequalsAscii(value, "page") || iequalsAscii(value, "left") ||
         iequalsAscii(value, "right") || iequalsAscii(value, "recto") || iequalsAscii(value, "verso");
}

// CSS pixels for a border width token (thin/medium/thick or a length); false if not a width.
bool interpretBorderWidth(const std::string_view token, uint8_t& out) {
  static constexpr std::string_view KEYWORDS[] = {"thin", "medium", "thick"};
  for (uint8_t i = 0; i < std::size(KEYWORDS); ++i) {
    if (iequalsAscii(token, KEYWORDS[i])) {
      out = i + 1;
      return true;
    }
  }
  size_t unitStart = token.size();
  for (size_t i = 0; i < token.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(token[i])) && token[i] != '.') {
      unitStart = i;
      break;
    }
  }
  float number = 0;
  if (unitStart == 0 || !tryParseNumber(token.substr(0, unitStart), number)) return false;
  const std::string_view unit = token.substr(unitStart);
  if (iequalsAscii(unit, "pt")) {
    number = number * 4.0f / 3.0f;
  } else if (iequalsAscii(unit, "em") || iequalsAscii(unit, "rem")) {
    number *= 16.0f;
  } else if (!unit.empty() && !iequalsAscii(unit, "px")) {
    return false;
  }
  // Hairlines stay visible; very thick borders are capped.
  out = number <= 0 ? 0 : static_cast<uint8_t>(std::clamp(number + 0.5f, 1.0f, 16.0f));
  return true;
}

bool interpretBorderStyle(const std::string_view token, CssBorderStyle& out) {
  if (iequalsAscii(token, "none") || iequalsAscii(token, "hidden")) {
    out = CssBorderStyle::None;
  } else if (iequalsAscii(token, "solid") || iequalsAscii(token, "groove") || iequalsAscii(token, "ridge") ||
             iequalsAscii(token, "inset") || iequalsAscii(token, "outset")) {
    out = CssBorderStyle::Solid;
  } else if (iequalsAscii(token, "double")) {
    out = CssBorderStyle::Double;
  } else if (iequalsAscii(token, "dotted")) {
    out = CssBorderStyle::Dotted;
  } else if (iequalsAscii(token, "dashed")) {
    out = CssBorderStyle::Dashed;
  } else {
    return false;
  }
  return true;
}

// "1px solid #000" -> width and style; colors are ignored (borders draw black).
CssBorderSide interpretBorderShorthand(const std::string_view value) {
  CssBorderSide side;
  bool hasWidth = false;
  forEachDelimitedToken(value, isCssWhitespace, [&](const std::string_view token) {
    uint8_t width = 0;
    CssBorderStyle style = CssBorderStyle::None;
    if (interpretBorderWidth(token, width)) {
      side.width = width;
      hasWidth = true;
    } else if (interpretBorderStyle(token, style)) {
      side.style = style;
    }
  });
  if (!hasWidth) side.width = 2;  // medium
  return side;
}

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  const char lower = asciiToLower(c);
  return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

// Perceived brightness (0-255) of the first color in a background value, or -1.
int colorLuminance(const std::string_view value) {
  const auto luminance = [](const int r, const int g, const int b) { return (r * 299 + g * 587 + b * 114) / 1000; };
  const size_t rgb = value.find("rgb");
  if (rgb != std::string_view::npos) {
    int channels[3] = {};
    int count = 0;
    int current = -1;
    for (size_t i = value.find('(', rgb); i != std::string_view::npos && i < value.size() && count < 3; ++i) {
      if (std::isdigit(static_cast<unsigned char>(value[i]))) {
        current = (current < 0 ? 0 : current * 10) + (value[i] - '0');
      } else if (current >= 0) {
        channels[count++] = value[i] == '%' ? current * 255 / 100 : std::min(current, 255);
        current = -1;
      }
    }
    return count == 3 ? luminance(channels[0], channels[1], channels[2]) : -1;
  }

  int result = -1;
  forEachDelimitedToken(value, isCssWhitespace, [&](const std::string_view token) {
    if (result >= 0) return;
    if (token.size() > 1 && token[0] == '#') {
      const std::string_view hex = token.substr(1);
      int c[3] = {-1, -1, -1};
      if (hex.size() == 3 || hex.size() == 4) {
        for (int i = 0; i < 3; ++i) c[i] = hexNibble(hex[i]) * 17;
      } else if (hex.size() == 6 || hex.size() == 8) {
        for (int i = 0; i < 3; ++i) c[i] = hexNibble(hex[2 * i]) * 16 + hexNibble(hex[2 * i + 1]);
      }
      if (c[0] >= 0 && c[1] >= 0 && c[2] >= 0) result = luminance(c[0], c[1], c[2]);
      return;
    }
    struct Named {
      std::string_view name;
      int luminance;
    };
    static constexpr Named NAMED[] = {
        {"white", 255},   {"whitesmoke", 245}, {"gainsboro", 220},   {"lightgray", 211}, {"lightgrey", 211},
        {"silver", 192},  {"darkgray", 169},   {"darkgrey", 169},    {"gray", 128},      {"grey", 128},
        {"dimgray", 105}, {"dimgrey", 105},    {"black", 0},         {"ivory", 254},     {"beige", 243},
        {"linen", 244},   {"lavender", 235},   {"transparent", 255},
    };
    for (const Named& named : NAMED) {
      if (iequalsAscii(token, named.name)) {
        result = named.luminance;
        return;
      }
    }
  });
  return result;
}

// Light backgrounds shade the box; white is invisible on paper, and dark ones
// are skipped because the text inside would stay black.
bool interpretShade(const std::string_view value) {
  const int luminance = colorLuminance(value);
  return luminance >= 128 && luminance < 240;
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
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

constexpr std::array STYLE_LENGTH_FIELDS = {
    &CssStyle::textIndent,   &CssStyle::marginTop,   &CssStyle::marginBottom,  &CssStyle::marginLeft,
    &CssStyle::marginRight,  &CssStyle::paddingTop,  &CssStyle::paddingBottom, &CssStyle::paddingLeft,
    &CssStyle::paddingRight, &CssStyle::imageHeight, &CssStyle::imageWidth,    &CssStyle::fontSize,
};
constexpr size_t STYLE_LENGTH_FIELD_COUNT = STYLE_LENGTH_FIELDS.size();
constexpr size_t STYLE_WIRE_BYTES =
    5 + STYLE_LENGTH_FIELD_COUNT * (sizeof(decltype(CssLength::value)) + 1) + 6 + 12 + sizeof(uint32_t);
constexpr uint32_t CSS_DEFINED_BITS_MASK = (1u << 31) - 1;
constexpr uint8_t MAX_INITIAL_LETTER = 6;

void encodeStyleWire(const CssStyle& style, uint8_t (&out)[STYLE_WIRE_BYTES]) {
  size_t offset = 0;
  out[offset++] = static_cast<uint8_t>(style.textAlign);
  out[offset++] = static_cast<uint8_t>(style.fontStyle);
  out[offset++] = static_cast<uint8_t>(style.fontWeight);
  out[offset++] = static_cast<uint8_t>(style.textDecoration);
  out[offset++] = static_cast<uint8_t>(style.direction);

  const auto putLength = [&out, &offset](const CssLength& length) {
    memcpy(out + offset, &length.value, sizeof(length.value));
    offset += sizeof(length.value);
    out[offset++] = static_cast<uint8_t>(length.unit);
  };
  for (const auto field : STYLE_LENGTH_FIELDS) {
    putLength(style.*field);
  }
  out[offset++] = static_cast<uint8_t>(style.display);
  out[offset++] = static_cast<uint8_t>(style.verticalAlign);
  out[offset++] = static_cast<uint8_t>(style.listStyleType);
  out[offset++] = style.smallCaps ? 1 : 0;
  out[offset++] = style.pageBreakBefore ? 1 : 0;
  out[offset++] = style.pageBreakAfter ? 1 : 0;
  for (const CssBorderSide* side : {&style.borderTop, &style.borderRight, &style.borderBottom, &style.borderLeft}) {
    out[offset++] = side->width;
    out[offset++] = static_cast<uint8_t>(side->style);
  }
  out[offset++] = style.shaded ? 1 : 0;
  out[offset++] = style.floatLeft ? 1 : 0;
  out[offset++] = style.initialLetter;
  out[offset++] = style.preserveWhitespace ? 1 : 0;

  uint32_t definedBits = 0;
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
  if (style.defined.direction) definedBits |= 1 << 16;
  if (style.defined.verticalAlign) definedBits |= 1 << 17;
  if (style.defined.listStyleType) definedBits |= 1 << 18;
  if (style.defined.fontSize) definedBits |= 1 << 19;
  if (style.defined.smallCaps) definedBits |= 1 << 20;
  if (style.defined.pageBreakBefore) definedBits |= 1 << 21;
  if (style.defined.pageBreakAfter) definedBits |= 1 << 22;
  if (style.defined.borderTop) definedBits |= 1 << 23;
  if (style.defined.borderRight) definedBits |= 1 << 24;
  if (style.defined.borderBottom) definedBits |= 1 << 25;
  if (style.defined.borderLeft) definedBits |= 1 << 26;
  if (style.defined.shaded) definedBits |= 1 << 27;
  if (style.defined.floatLeft) definedBits |= 1 << 28;
  if (style.defined.initialLetter) definedBits |= 1 << 29;
  if (style.defined.whiteSpace) definedBits |= 1u << 30;
  memcpy(out + offset, &definedBits, sizeof(definedBits));
}

bool decodeStyleWire(const uint8_t (&in)[STYLE_WIRE_BYTES], CssStyle& style) {
  size_t offset = 0;
  const uint8_t textAlign = in[offset++];
  const uint8_t fontStyle = in[offset++];
  const uint8_t fontWeight = in[offset++];
  const uint8_t textDecoration = in[offset++];
  const uint8_t direction = in[offset++];
  if (textAlign > static_cast<uint8_t>(CssTextAlign::None) || fontStyle > static_cast<uint8_t>(CssFontStyle::Italic) ||
      fontWeight > static_cast<uint8_t>(CssFontWeight::Bold) || (textDecoration & ~CSS_TEXT_DECORATION_MASK) != 0 ||
      direction > static_cast<uint8_t>(CssTextDirection::Rtl)) {
    return false;
  }
  style.textAlign = static_cast<CssTextAlign>(textAlign);
  style.fontStyle = static_cast<CssFontStyle>(fontStyle);
  style.fontWeight = static_cast<CssFontWeight>(fontWeight);
  style.textDecoration = static_cast<CssTextDecoration>(textDecoration);
  style.direction = static_cast<CssTextDirection>(direction);

  const auto getLength = [&in, &offset](CssLength& length) {
    decltype(CssLength::value) value = 0;
    memcpy(&value, in + offset, sizeof(value));
    offset += sizeof(length.value);
    const uint8_t unit = in[offset++];
    if (!std::isfinite(value) || unit > static_cast<uint8_t>(CssUnit::Percent)) return false;
    length.value = value;
    length.unit = static_cast<CssUnit>(unit);
    return true;
  };
  for (const auto field : STYLE_LENGTH_FIELDS) {
    if (!getLength(style.*field)) return false;
  }

  const uint8_t display = in[offset++];
  const uint8_t verticalAlign = in[offset++];
  const uint8_t listStyleType = in[offset++];
  if (display > static_cast<uint8_t>(CssDisplay::None) || verticalAlign > static_cast<uint8_t>(CssVerticalAlign::Sub) ||
      listStyleType > static_cast<uint8_t>(CssListStyleType::None)) {
    return false;
  }
  style.display = static_cast<CssDisplay>(display);
  style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlign);
  style.listStyleType = static_cast<CssListStyleType>(listStyleType);

  const uint8_t smallCaps = in[offset++];
  const uint8_t pageBreakBefore = in[offset++];
  const uint8_t pageBreakAfter = in[offset++];
  if (smallCaps > 1 || pageBreakBefore > 1 || pageBreakAfter > 1) return false;
  style.smallCaps = smallCaps != 0;
  style.pageBreakBefore = pageBreakBefore != 0;
  style.pageBreakAfter = pageBreakAfter != 0;

  for (CssBorderSide* side : {&style.borderTop, &style.borderRight, &style.borderBottom, &style.borderLeft}) {
    side->width = in[offset++];
    const uint8_t borderStyle = in[offset++];
    if (borderStyle > static_cast<uint8_t>(CssBorderStyle::Dashed)) return false;
    side->style = static_cast<CssBorderStyle>(borderStyle);
  }
  const uint8_t shaded = in[offset++];
  const uint8_t floatLeft = in[offset++];
  const uint8_t initialLetter = in[offset++];
  if (shaded > 1 || floatLeft > 1 || initialLetter > MAX_INITIAL_LETTER) return false;
  style.shaded = shaded != 0;
  style.floatLeft = floatLeft != 0;
  style.initialLetter = initialLetter;
  const uint8_t preserveWhitespace = in[offset++];
  if (preserveWhitespace > 1) return false;
  style.preserveWhitespace = preserveWhitespace != 0;

  uint32_t definedBits = 0;
  memcpy(&definedBits, in + offset, sizeof(definedBits));
  if ((definedBits & ~CSS_DEFINED_BITS_MASK) != 0) return false;
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
  style.defined.direction = (definedBits & 1 << 16) != 0;
  style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
  style.defined.listStyleType = (definedBits & 1 << 18) != 0;
  style.defined.fontSize = (definedBits & 1 << 19) != 0;
  style.defined.smallCaps = (definedBits & 1 << 20) != 0;
  style.defined.pageBreakBefore = (definedBits & 1 << 21) != 0;
  style.defined.pageBreakAfter = (definedBits & 1 << 22) != 0;
  style.defined.borderTop = (definedBits & 1 << 23) != 0;
  style.defined.borderRight = (definedBits & 1 << 24) != 0;
  style.defined.borderBottom = (definedBits & 1 << 25) != 0;
  style.defined.borderLeft = (definedBits & 1 << 26) != 0;
  style.defined.shaded = (definedBits & 1 << 27) != 0;
  style.defined.floatLeft = (definedBits & 1 << 28) != 0;
  style.defined.initialLetter = (definedBits & 1 << 29) != 0;
  style.defined.whiteSpace = (definedBits & 1u << 30) != 0;
  return true;
}

}  // anonymous namespace

int CssParser::compareEntryToPieces(const SelectorEntry& entry, const KeyPieces& key, const bool prefixOnly) const {
  const char* stored = selectorPool_.get() + entry.offset;
  size_t index = 0;
  for (uint8_t p = 0; p < key.count; ++p) {
    for (const char c : key.piece[p]) {
      if (index == entry.length) return -1;
      const auto storedByte = static_cast<unsigned char>(stored[index]);
      const auto probeByte = static_cast<unsigned char>(asciiToLower(c));
      if (storedByte != probeByte) return storedByte < probeByte ? -1 : 1;
      ++index;
    }
  }
  return prefixOnly || index == entry.length ? 0 : 1;
}

size_t CssParser::lowerBound(const KeyPieces& key, bool& exact) const {
  size_t low = 0;
  size_t high = entryCount_;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (compareEntryToPieces(entries_[middle], key) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  exact = low < entryCount_ && compareEntryToPieces(entries_[low], key) == 0;
  return low;
}

std::string_view CssParser::selectorAt(const size_t index) const {
  const SelectorEntry& entry = entries_[index];
  return {selectorPool_.get() + entry.offset, entry.length};
}

CssParser::PoolResult CssParser::ensureEntryCapacity(const size_t needed) {
  if (needed <= entryCapacity_) return PoolResult::Ready;
  if (needed > MAX_RULES) return PoolResult::Limit;

  size_t capacity = entryCapacity_ ? entryCapacity_ * 2u : 32u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, MAX_RULES);
  auto grown = makeUniqueNoThrow<SelectorEntry[]>(capacity);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector index (%zu entries)", capacity);
    return PoolResult::OutOfMemory;
  }
  if (entryCount_ > 0) memcpy(grown.get(), entries_.get(), entryCount_ * sizeof(SelectorEntry));
  entries_ = std::move(grown);
  entryCapacity_ = static_cast<uint16_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::ensureSelectorPoolCapacity(const size_t needed) {
  if (needed <= selectorPoolCapacity_) return PoolResult::Ready;
  if (needed > SELECTOR_POOL_CAP) return PoolResult::Limit;

  size_t capacity = selectorPoolCapacity_ ? selectorPoolCapacity_ * 2u : 512u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, SELECTOR_POOL_CAP);
  auto grown = makeUniqueNoThrow<char[]>(capacity);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector pool (%zu bytes)", capacity);
    return PoolResult::OutOfMemory;
  }
  if (selectorPoolSize_ > 0) memcpy(grown.get(), selectorPool_.get(), selectorPoolSize_);
  selectorPool_ = std::move(grown);
  selectorPoolCapacity_ = static_cast<uint32_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::ensureStyleCapacity(const size_t needed) {
  if (needed <= styleCapacity_) return PoolResult::Ready;
  if (needed > MAX_UNIQUE_STYLES) return PoolResult::Limit;

  size_t capacity = styleCapacity_ ? styleCapacity_ * 2u : 16u;
  while (capacity < needed) capacity *= 2u;
  capacity = std::min(capacity, MAX_UNIQUE_STYLES);
  auto grownStyles = makeUniqueNoThrow<CssStyle[]>(capacity);
  if (!grownStyles) {
    LOG_ERR("CSS", "OOM: style pool (%zu styles)", capacity);
    return PoolResult::OutOfMemory;
  }
  for (size_t i = 0; i < styleCount_; ++i) grownStyles[i] = stylePool_[i];
  stylePool_ = std::move(grownStyles);
  styleCapacity_ = static_cast<uint16_t>(capacity);
  return PoolResult::Ready;
}

CssParser::PoolResult CssParser::internStyle(const CssStyle& style, uint16_t& indexOut) {
  uint8_t wire[STYLE_WIRE_BYTES];
  encodeStyleWire(style, wire);
  for (uint16_t i = 0; i < styleCount_; ++i) {
    uint8_t existingWire[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[i], existingWire);
    if (memcmp(existingWire, wire, STYLE_WIRE_BYTES) == 0) {
      indexOut = i;
      return PoolResult::Ready;
    }
  }

  const PoolResult capacityResult = ensureStyleCapacity(static_cast<size_t>(styleCount_) + 1);
  if (capacityResult != PoolResult::Ready) return capacityResult;
  stylePool_[styleCount_] = style;
  indexOut = styleCount_++;
  return PoolResult::Ready;
}

void CssParser::noteRuleShape(const std::string_view storedKey) {
  const size_t separator = storedKey.find(CONTEXT_SEPARATOR);
  hasContextualRules_ = hasContextualRules_ || separator != std::string_view::npos;
  const std::string_view subject = storedKey.substr(0, separator);
  hasIdRules_ = hasIdRules_ || subject.find('#') != std::string_view::npos;
  hasCompoundRules_ = hasCompoundRules_ || std::count(subject.begin(), subject.end(), '.') >= 2;
  hasFirstLetterRules_ = hasFirstLetterRules_ || subject.find(':') != std::string_view::npos;
}

CssParser::RuleInsertResult CssParser::insertOrMerge(const KeyPieces& key, const CssStyle& style) {
  bool exact = false;
  const size_t position = lowerBound(key, exact);
  // Only adjacent rules can merge without moving older declarations past other selectors.
  if (exact && entries_[position].offset + entries_[position].length == selectorPoolSize_) {
    const uint16_t currentStyleIndex = entries_[position].styleIndex;
    CssStyle merged = stylePool_[currentStyleIndex];
    merged.applyOver(style);

    bool styleIsShared = false;
    for (uint16_t i = 0; i < entryCount_; ++i) {
      if (i != position && entries_[i].styleIndex == currentStyleIndex) {
        styleIsShared = true;
        break;
      }
    }
    if (!styleIsShared) {
      stylePool_[currentStyleIndex] = merged;
      return RuleInsertResult::Merged;
    }

    uint16_t styleIndex = 0;
    const PoolResult result = internStyle(merged, styleIndex);
    if (result == PoolResult::Limit) return RuleInsertResult::Limit;
    if (result == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;
    entries_[position].styleIndex = styleIndex;
    return RuleInsertResult::Merged;
  }

  const PoolResult entryResult = ensureEntryCapacity(static_cast<size_t>(entryCount_) + 1);
  if (entryResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (entryResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  const size_t requiredSelectorBytes = static_cast<size_t>(selectorPoolSize_) + key.length;
  const PoolResult selectorResult = ensureSelectorPoolCapacity(requiredSelectorBytes);
  if (selectorResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (selectorResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  uint16_t styleIndex = 0;
  const PoolResult styleResult = internStyle(style, styleIndex);
  if (styleResult == PoolResult::Limit) return RuleInsertResult::Limit;
  if (styleResult == PoolResult::OutOfMemory) return RuleInsertResult::OutOfMemory;

  const uint32_t selectorOffset = selectorPoolSize_;
  char* destination = selectorPool_.get() + selectorOffset;
  for (uint8_t p = 0; p < key.count; ++p) {
    for (const char c : key.piece[p]) *destination++ = asciiToLower(c);
  }
  selectorPoolSize_ = static_cast<uint32_t>(requiredSelectorBytes);

  SelectorEntry* entries = entries_.get();
  memmove(entries + position + 1, entries + position, (entryCount_ - position) * sizeof(SelectorEntry));
  entries[position] = {selectorOffset, styleIndex, static_cast<uint16_t>(key.length)};
  ++entryCount_;
  noteRuleShape(selectorAt(position));
  return RuleInsertResult::Inserted;
}

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values. Compare whole tokens
  // so malformed values like "notunderline" do not accidentally enable a line.
  CssTextDecoration result = CssTextDecoration::None;
  bool explicitNone = false;
  forEachDelimitedToken(val, isCssWhitespace, [&](const std::string_view token) {
    if (iequalsAscii(token, "none")) {
      explicitNone = true;
    } else if (iequalsAscii(token, "underline")) {
      result = result | CssTextDecoration::Underline;
    } else if (iequalsAscii(token, "line-through")) {
      result = result | CssTextDecoration::LineThrough;
    }
  });
  return explicitNone ? CssTextDecoration::None : result;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseNumber(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

bool CssParser::tryInterpretFontSize(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);

  struct Keyword {
    std::string_view name;
    float scale;
    CssUnit unit;
  };
  static constexpr Keyword KEYWORDS[] = {
      {"xx-small", 0.6f, CssUnit::Rem}, {"x-small", 0.75f, CssUnit::Rem},  {"small", 0.89f, CssUnit::Rem},
      {"medium", 1.0f, CssUnit::Rem},   {"large", 1.2f, CssUnit::Rem},     {"x-large", 1.5f, CssUnit::Rem},
      {"xx-large", 2.0f, CssUnit::Rem}, {"xxx-large", 3.0f, CssUnit::Rem}, {"smaller", 0.83f, CssUnit::Em},
      {"larger", 1.2f, CssUnit::Em},
  };
  for (const Keyword& keyword : KEYWORDS) {
    if (iequalsAscii(val, keyword.name)) {
      out = CssLength{keyword.scale, keyword.unit};
      return true;
    }
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(val[i])) && val[i] != '.' && val[i] != '+' && val[i] != '-') {
      unitStart = i;
      break;
    }
  }
  float number = 0;
  if (!tryParseNumber(val.substr(0, unitStart), number) || !(number > 0)) return false;

  // Absolute sizes resolve against the CSS default body size: 16px = 12pt = 1rem.
  const std::string_view unit = val.substr(unitStart);
  if (unit.empty() || iequalsAscii(unit, "px")) {
    out = CssLength{number / 16.0f, CssUnit::Rem};
  } else if (iequalsAscii(unit, "pt")) {
    out = CssLength{number / 12.0f, CssUnit::Rem};
  } else if (iequalsAscii(unit, "rem")) {
    out = CssLength{number, CssUnit::Rem};
  } else if (iequalsAscii(unit, "em")) {
    out = CssLength{number, CssUnit::Em};
  } else if (unit == "%") {
    out = CssLength{number / 100.0f, CssUnit::Em};
  } else if (iequalsAscii(unit, "ex") || iequalsAscii(unit, "ch")) {
    out = CssLength{number / 2.0f, CssUnit::Em};
  } else {
    return false;
  }
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  value = stripTrailingImportant(value);

  if (iequalsAscii(name, "white-space")) {
    const bool preserve =
        iequalsAscii(value, "pre-wrap") || iequalsAscii(value, "pre") || iequalsAscii(value, "break-spaces");
    if (preserve || iequalsAscii(value, "normal")) {
      style.preserveWhitespace = preserve;
      style.defined.whiteSpace = 1;
    }
  } else if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    style.display = iequalsAscii(value, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "direction")) {
    if (iequalsAscii(value, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(value, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  } else if (iequalsAscii(name, "font-size")) {
    CssLength size;
    if (tryInterpretFontSize(value, size)) {
      style.fontSize = size;
      style.defined.fontSize = 1;
    }
  } else if (iequalsAscii(name, "font-variant") || iequalsAscii(name, "font-variant-caps")) {
    bool smallCaps = false;
    forEachDelimitedToken(value, isCssWhitespace, [&](const std::string_view token) {
      smallCaps = smallCaps || iequalsAscii(token, "small-caps") || iequalsAscii(token, "all-small-caps");
    });
    style.smallCaps = smallCaps;
    style.defined.smallCaps = 1;
  } else if (iequalsAscii(name, "page-break-before") || iequalsAscii(name, "break-before")) {
    style.pageBreakBefore = interpretForcedBreak(value);
    style.defined.pageBreakBefore = 1;
  } else if (iequalsAscii(name, "page-break-after") || iequalsAscii(name, "break-after")) {
    style.pageBreakAfter = interpretForcedBreak(value);
    style.defined.pageBreakAfter = 1;
  } else if (name.size() >= 6 && iequalsAscii(name.substr(0, 6), "border")) {
    parseBorderDeclaration(name, value, style);
  } else if (iequalsAscii(name, "background-color") || iequalsAscii(name, "background")) {
    style.shaded = interpretShade(value);
    style.defined.shaded = 1;
  } else if (iequalsAscii(name, "float")) {
    style.floatLeft = iequalsAscii(value, "left");
    style.defined.floatLeft = 1;
  } else if (iequalsAscii(name, "initial-letter")) {
    uint32_t lines = 0;
    forEachDelimitedToken(value, isCssWhitespace, [&](const std::string_view token) {
      float number = 0;
      if (lines == 0 && tryParseNumber(token, number) && number >= 1) lines = static_cast<uint32_t>(number + 0.5f);
    });
    style.initialLetter = static_cast<uint8_t>(std::min<uint32_t>(lines, MAX_INITIAL_LETTER));
    style.defined.initialLetter = 1;
  } else if (iequalsAscii(name, "list-style-type")) {
    const std::string_view listStyleValue = stripTrailingImportant(value);
    style.listStyleType = iequalsAscii(listStyleValue, "none") ? CssListStyleType::None : CssListStyleType::Disc;
    style.defined.listStyleType = 1;
  }
}

void CssParser::parseBorderDeclaration(const std::string_view name, const std::string_view value, CssStyle& style) {
  CssBorderSide* sides[4] = {&style.borderTop, &style.borderRight, &style.borderBottom, &style.borderLeft};
  const auto markDefined = [&style](const size_t side) {
    if (side == 0) style.defined.borderTop = 1;
    if (side == 1) style.defined.borderRight = 1;
    if (side == 2) style.defined.borderBottom = 1;
    if (side == 3) style.defined.borderLeft = 1;
  };
  static constexpr std::string_view SIDE_NAMES[] = {"top", "right", "bottom", "left"};

  if (iequalsAscii(name, "border")) {
    const CssBorderSide side = interpretBorderShorthand(value);
    for (size_t i = 0; i < 4; ++i) {
      *sides[i] = side;
      markDefined(i);
    }
    return;
  }

  // border-width / border-style take 1-4 edge values (top, right, bottom, left).
  const bool edgeWidths = iequalsAscii(name, "border-width");
  if (edgeWidths || iequalsAscii(name, "border-style")) {
    std::string_view tokens[4];
    const size_t count = collectEdgeValueTokens(value, tokens);
    if (count == 0) return;
    const size_t source[4] = {0, count >= 2 ? 1u : 0u, count >= 3 ? 2u : 0u, count >= 4 ? 3u : (count >= 2 ? 1u : 0u)};
    for (size_t i = 0; i < 4; ++i) {
      const std::string_view token = tokens[source[i]];
      if (edgeWidths ? interpretBorderWidth(token, sides[i]->width) : interpretBorderStyle(token, sides[i]->style)) {
        markDefined(i);
      }
    }
    return;
  }

  // border-<side>, border-<side>-width, border-<side>-style
  if (name.size() <= 7 || name[6] != '-') return;
  const std::string_view prefix = name.substr(7);  // after "border-"
  for (size_t i = 0; i < 4; ++i) {
    if (prefix.size() < SIDE_NAMES[i].size() || !iequalsAscii(prefix.substr(0, SIDE_NAMES[i].size()), SIDE_NAMES[i])) {
      continue;
    }
    const std::string_view suffix = prefix.substr(SIDE_NAMES[i].size());
    if (suffix.empty()) {
      *sides[i] = interpretBorderShorthand(value);
      markDefined(i);
    } else if (iequalsAscii(suffix, "-width")) {
      if (interpretBorderWidth(trimCssWhitespace(value), sides[i]->width)) markDefined(i);
    } else if (iequalsAscii(suffix, "-style")) {
      if (interpretBorderStyle(trimCssWhitespace(value), sides[i]->style)) markDefined(i);
    }
    return;
  }
}

CssStyle CssParser::parseDeclarations(std::string_view declBlock) {
  CssStyle style;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        parseDeclarationIntoStyle(declBlock.substr(start, i - start), style);
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // Skip rules that don't define any supported properties to save RAM.
  if (!style.defined.anySet()) {
    return;
  }

  // Walk comma-separated selectors in place. The bounded store reports every
  // capacity or allocation failure without crossing a throwing STL boundary.
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // ::first-letter (or legacy :first-letter) rules are stored under their own key.
        bool firstLetter = false;
        for (const std::string_view suffix : {FIRST_LETTER_MARKER, FIRST_LETTER_MARKER.substr(1)}) {
          if (!firstLetter && sel.size() > suffix.size() &&
              iequalsAscii(sel.substr(sel.size() - suffix.size()), suffix)) {
            sel.remove_suffix(suffix.size());
            firstLetter = true;
          }
        }

        // Sibling combinators, attribute selectors and other pseudo-classes/elements are unsupported.
        constexpr std::string_view kUnsupportedSelectorChars = "+~[:";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;

        std::string_view context = sel;
        CompoundSelector subject;
        if (!parseCompound(popTrailingCompound(context), subject) || subject.empty()) return;
        bool hasAncestor = false;
        for (std::string_view rest = context;;) {
          popTrailingCombinator(rest);
          if (rest.empty()) break;
          CompoundSelector ancestor;
          if (!parseCompound(popTrailingCompound(rest), ancestor)) return;
          hasAncestor = true;
        }
        if (!context.empty() && !hasAncestor) return;

        // Canonical subject: element, #id, then classes in sorted order, which is
        // the order resolveStyle enumerates them in.
        sortIgnoringAsciiCase(subject.classes, subject.classCount);
        KeyPieces key;
        key.add(subject.tag);
        if (!subject.id.empty()) {
          key.add(ID_MARKER);
          key.add(subject.id);
        }
        for (uint8_t i = 0; i < subject.classCount; ++i) {
          key.add(CLASS_MARKER);
          key.add(subject.classes[i]);
        }
        if (firstLetter) key.add(FIRST_LETTER_MARKER);
        if (hasAncestor) {
          key.add(CONTEXT_SEPARATOR);
          key.add(context);
        }

        if (ruleGrowthStopped_) {
          // Continue the cascade for stored selectors without retrying failed
          // allocations for new rules.
          bool exact = false;
          const size_t matchingIndex = lowerBound(key, exact);
          if (!exact || matchingIndex >= entryCount_) return;
        }
        const RuleInsertResult result = insertOrMerge(key, style);
        if (result == RuleInsertResult::Limit) {
          LOG_ERR("CSS", "CSS rule store limit reached at %u rules", entryCount_);
          ruleGrowthStopped_ = true;
        } else if (result == RuleInsertResult::OutOfMemory) {
          LOG_ERR("CSS", "OOM while growing CSS rule store at %u rules", entryCount_);
          ruleGrowthStopped_ = true;
        }
      });
}

// Main parsing entry point

CssParser::ParseResult CssParser::loadFromStream(HalFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return ParseResult::Error;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  bool selectorTruncated = false;
  bool declarationTruncated = false;
  bool inputTruncated = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
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
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        skippingRule = selectorTruncated || selector.size() > MAX_SELECTOR_LENGTH * 4;
        return;
      }
      if (!selector.push_back(c)) {
        selectorTruncated = true;
        inputTruncated = true;
      }
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
        if (!skippingRule && !declarationTruncated && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        selectorTruncated = false;
        declarationTruncated = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declarationTruncated && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
        }
        declBuffer.clear();
        declarationTruncated = false;
      } else {
        if (!declBuffer.push_back(c)) {
          declarationTruncated = true;
          inputTruncated = true;
        }
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

  if (inputTruncated) {
    LOG_ERR("CSS", "CSS input exceeded parser buffer; cache will remain partial");
  }
  const bool incompleteInput = bodyDepth > 0 || inAtRule || inComment || !selector.empty();
  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", ruleCount(), totalRead);
  return ruleGrowthStopped_ || inputTruncated || incompleteInput ? ParseResult::Partial : ParseResult::Complete;
}

// Style resolution

CssAncestor CssParser::makeAncestor(const std::string_view tagName, const std::string_view classAttr,
                                    const std::string_view idAttr) {
  CssAncestor ancestor;
  ancestor.tagHash = hashIgnoringAsciiCase(tagName);
  ancestor.idHash = idAttr.empty() ? 0 : hashIgnoringAsciiCase(idAttr);
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](const std::string_view cls) {
    if (ancestor.classCount < CssAncestor::MAX_CLASSES) {
      ancestor.classHashes[ancestor.classCount++] = hashIgnoringAsciiCase(cls);
    }
  });
  return ancestor;
}

CssStyle CssParser::resolveStyle(const std::string_view tagName, const std::string_view classAttr,
                                 const std::string_view idAttr, const CssAncestor* ancestors,
                                 const size_t ancestorCount, const bool firstLetter) const {
  static bool lowHeapWarningLogged = false;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), returning empty style",
              ESP.getFreeHeap(), static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }

  CssStyle result;
  if (entryCount_ == 0 || (firstLetter && !hasFirstLetterRules_)) return result;

  std::string_view classes[MAX_ELEMENT_CLASSES];
  size_t classCount = 0;
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](const std::string_view cls) {
    if (classCount < MAX_ELEMENT_CLASSES) classes[classCount++] = cls;
  });
  sortIgnoringAsciiCase(classes, classCount);
  classCount = static_cast<size_t>(std::unique(classes, classes + classCount,
                                               [](const std::string_view a, const std::string_view b) {
                                                 return !lessIgnoringAsciiCase(a, b) && !lessIgnoringAsciiCase(b, a);
                                               }) -
                                   classes);

  struct Match {
    uint32_t specificity;
    uint16_t entryIndex;
  };
  Match matches[MAX_STYLE_MATCHES];
  size_t matchCount = 0;
  Match lastApplied{};
  bool hasLastApplied = false;
  // Selector text is appended in source order, including across stylesheets.
  const auto precedes = [this](const Match& a, const Match& b) {
    return a.specificity < b.specificity ||
           (a.specificity == b.specificity && entries_[a.entryIndex].offset < entries_[b.entryIndex].offset);
  };
  const auto addMatch = [&](const uint32_t specificity, const uint16_t entryIndex) {
    const Match match{specificity, entryIndex};
    if (hasLastApplied && !precedes(lastApplied, match)) return;
    size_t position = 0;
    while (position < matchCount && precedes(matches[position], match)) ++position;
    if (position == MAX_STYLE_MATCHES) return;
    if (matchCount < MAX_STYLE_MATCHES) ++matchCount;
    for (size_t i = matchCount - 1; i > position; --i) matches[i] = matches[i - 1];
    matches[position] = match;
  };

  KeyPieces key;
  const auto lookup = [&](const uint32_t specificity) {
    bool exact = false;
    const size_t position = lowerBound(key, exact);
    for (size_t i = position; i < entryCount_ && compareEntryToPieces(entries_[i], key) == 0; ++i) {
      addMatch(specificity, static_cast<uint16_t>(i));
    }
    if (!hasContextualRules_ || ancestorCount == 0 || !key.add(CONTEXT_SEPARATOR)) return;
    for (size_t i = lowerBound(key, exact); i < entryCount_ && compareEntryToPieces(entries_[i], key, true) == 0; ++i) {
      const std::string_view context = selectorAt(i).substr(key.length);
      if (ancestorsMatch(context, ancestors, ancestorCount)) {
        addMatch(specificity + contextSpecificity(context), static_cast<uint16_t>(i));
      }
    }
    key.count--;
    key.length -= CONTEXT_SEPARATOR.size();
  };

  const size_t subsetClassCount = std::min(classCount, MAX_COMPOUND_SUBSET_CLASSES);
  const bool useIdRules = hasIdRules_ && !idAttr.empty();
  // Process additional matches in bounded batches instead of dropping repeated rules.
  do {
    matchCount = 0;
    for (uint32_t withId = 0; withId <= (useIdRules ? 1u : 0u); ++withId) {
      for (uint32_t withTag = 0; withTag <= 1; ++withTag) {
        key = KeyPieces{};
        if (withTag) key.add(tagName);
        if (withId) {
          key.add(ID_MARKER);
          key.add(idAttr);
        }
        const KeyPieces base = key;
        const auto addPseudo = [&] {
          if (firstLetter) key.add(FIRST_LETTER_MARKER);
        };
        if (withTag || withId) {
          addPseudo();
          lookup(packSpecificity(withId, 0, withTag));
        }

        for (size_t i = 0; i < classCount; ++i) {
          key = base;
          key.add(CLASS_MARKER);
          key.add(classes[i]);
          addPseudo();
          lookup(packSpecificity(withId, 1, withTag));
        }

        if (!hasCompoundRules_) continue;
        for (uint32_t mask = 1; mask < (1u << subsetClassCount); ++mask) {
          const auto bits = static_cast<uint32_t>(__builtin_popcount(mask));
          if (bits < 2) continue;
          key = base;
          for (size_t i = 0; i < subsetClassCount; ++i) {
            if (mask & (1u << i)) {
              key.add(CLASS_MARKER);
              key.add(classes[i]);
            }
          }
          addPseudo();
          lookup(packSpecificity(withId, bits, withTag));
        }
      }
    }

    for (size_t i = 0; i < matchCount; ++i) {
      result.applyOver(stylePool_[entries_[matches[i].entryIndex].styleIndex]);
    }
    if (matchCount > 0) {
      lastApplied = matches[matchCount - 1];
      hasLastApplied = true;
    }
  } while (matchCount == MAX_STYLE_MATCHES);
  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char rulesCacheTmp[] = "/css_rules.cache.tmp";
constexpr char rulesCacheBackup[] = "/css_rules.cache.bak";
constexpr uint8_t CSS_CACHE_FLAG_PARTIAL = 1 << 0;
constexpr uint8_t CSS_CACHE_KNOWN_FLAGS = CSS_CACHE_FLAG_PARTIAL;

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

bool CssParser::restoreCacheBackupIfNeeded() const {
  if (cachePath.empty()) {
    return false;
  }

  const std::string finalPath = cachePath + rulesCache;
  if (Storage.exists(finalPath.c_str())) {
    return true;
  }

  const std::string backupPath = cachePath + rulesCacheBackup;
  if (!Storage.exists(backupPath.c_str())) {
    return false;
  }

  if (!Storage.rename(backupPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to restore CSS cache backup");
    return false;
  }

  LOG_DBG("CSS", "Restored CSS cache backup after interrupted replacement");
  return true;
}

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  Storage.remove((cachePath + rulesCacheTmp).c_str());
  Storage.remove((cachePath + rulesCacheBackup).c_str());
}

CssParser::CacheStatus CssParser::inspectCache() const {
  if (cachePath.empty() || (!hasCache() && !restoreCacheBackupIfNeeded())) {
    return CacheStatus::Missing;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return CacheStatus::Invalid;
  }

  uint8_t version = 0;
  uint8_t flags = 0;
  uint16_t ruleCount = 0;
  if (file.read(&version, sizeof(version)) != sizeof(version) || version != CSS_CACHE_VERSION ||
      file.read(&flags, sizeof(flags)) != sizeof(flags) || (flags & ~CSS_CACHE_KNOWN_FLAGS) != 0 ||
      file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount) || ruleCount > MAX_RULES) {
    return CacheStatus::Invalid;
  }

  const bool partial = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;
  if (!partial) {
    // Complete caches are fully validated while hydrating, avoiding a second
    // payload scan on every EPUB open.
    return CacheStatus::Complete;
  }

  const auto skipBytes = [&file](const size_t byteCount) {
    return static_cast<size_t>(file.available()) >= byteCount && file.seekCur(byteCount);
  };
  size_t selectorBytes = 0;
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) || selectorLen == 0 ||
        selectorLen > MAX_STORED_KEY_LENGTH) {
      return CacheStatus::Invalid;
    }
    selectorBytes += selectorLen;
    if (selectorBytes > SELECTOR_POOL_CAP || !skipBytes(static_cast<size_t>(selectorLen) + STYLE_WIRE_BYTES)) {
      return CacheStatus::Invalid;
    }
  }

  if (file.available() != 0) {
    return CacheStatus::Invalid;
  }

  return CacheStatus::Partial;
}

bool CssParser::saveToCache(const bool complete) const {
  if (cachePath.empty()) {
    return false;
  }

  // Preserve source order on reload; the bounded index can exceed the task's stack budget.
  auto sourceOrder = entryCount_ > 0 ? makeUniqueNoThrow<uint16_t[]>(entryCount_) : nullptr;
  if (entryCount_ > 0 && !sourceOrder) {
    LOG_ERR("CSS", "OOM sorting CSS cache rules");
    return false;
  }
  if (entryCount_ > 0) {
    for (uint16_t i = 0; i < entryCount_; ++i) sourceOrder[i] = i;
    std::sort(sourceOrder.get(), sourceOrder.get() + entryCount_,
              [this](const uint16_t a, const uint16_t b) { return entries_[a].offset < entries_[b].offset; });
  }

  const std::string finalPath = cachePath + rulesCache;
  const std::string tmpPath = cachePath + rulesCacheTmp;
  const std::string backupPath = cachePath + rulesCacheBackup;

  Storage.remove(tmpPath.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("CSS", tmpPath, file)) {
    return false;
  }

  bool writeOk = true;
  const auto writeBytes = [&file, &writeOk](const void* data, const size_t size) {
    if (writeOk && size > 0 && file.write(data, size) != size) {
      writeOk = false;
    }
  };
  const auto writeByte = [&writeBytes](const uint8_t value) { writeBytes(&value, sizeof(value)); };

  writeByte(CssParser::CSS_CACHE_VERSION);

  // A partial cache can style the current low-memory session, but the next
  // EPUB load must retry the source stylesheets instead of trusting it.
  writeByte(complete ? 0 : CSS_CACHE_FLAG_PARTIAL);

  // Write rule count
  const uint16_t ruleCount = entryCount_;
  writeBytes(&ruleCount, sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (uint16_t i = 0; i < entryCount_; ++i) {
    const uint16_t entryIndex = sourceOrder[i];
    const std::string_view selector = selectorAt(entryIndex);
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(selector.size());
    writeBytes(&selectorLen, sizeof(selectorLen));
    writeBytes(selector.data(), selectorLen);

    uint8_t styleWire[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[entries_[entryIndex].styleIndex], styleWire);
    writeBytes(styleWire, sizeof(styleWire));
    if (!writeOk) break;
  }

  if (!writeOk || !file.close()) {
    LOG_ERR("CSS", "Failed to write temporary CSS cache");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  const bool hadExistingCache = Storage.exists(finalPath.c_str());
  if (hadExistingCache) {
    Storage.remove(backupPath.c_str());
    if (!Storage.rename(finalPath.c_str(), backupPath.c_str())) {
      LOG_ERR("CSS", "Failed to back up existing CSS cache");
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to promote temporary CSS cache");
    Storage.remove(tmpPath.c_str());
    if (Storage.exists(backupPath.c_str()) && !Storage.rename(backupPath.c_str(), finalPath.c_str())) {
      LOG_ERR("CSS", "Failed to restore previous CSS cache");
    }
    return false;
  }

  Storage.remove(backupPath.c_str());

  LOG_DBG("CSS", "Saved %u rules to %s cache", ruleCount, complete ? "complete" : "partial");
  return true;
}

CssParser::CacheLoadResult CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return CacheLoadResult::Invalid;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    if (!restoreCacheBackupIfNeeded() || !Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
      return CacheLoadResult::Invalid;
    }
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return CacheLoadResult::Invalid;
  }

  uint8_t flags = 0;
  if (file.read(&flags, sizeof(flags)) != sizeof(flags) || (flags & ~CSS_CACHE_KNOWN_FLAGS) != 0) {
    LOG_DBG("CSS", "Invalid CSS cache flags: %u", flags);
    return CacheLoadResult::Invalid;
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return CacheLoadResult::Invalid;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    clear();
    return CacheLoadResult::Invalid;
  }

  const PoolResult entryCapacityResult = ensureEntryCapacity(ruleCount);
  if (entryCapacityResult == PoolResult::OutOfMemory) {
    clear();
    return CacheLoadResult::LowMemory;
  }
  if (entryCapacityResult == PoolResult::Limit) {
    clear();
    return CacheLoadResult::Invalid;
  }

  auto selectorBuffer = ruleCount > 0 ? makeUniqueNoThrow<char[]>(MAX_STORED_KEY_LENGTH) : nullptr;
  if (ruleCount > 0 && !selectorBuffer) {
    clear();
    return CacheLoadResult::LowMemory;
  }

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    // Read selector string
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    if (selectorLen == 0 || selectorLen > MAX_STORED_KEY_LENGTH) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      clear();
      return CacheLoadResult::Invalid;
    }

    if (file.read(selectorBuffer.get(), selectorLen) != selectorLen) {
      clear();
      return CacheLoadResult::Invalid;
    }

    uint8_t styleWire[STYLE_WIRE_BYTES];
    if (file.read(styleWire, sizeof(styleWire)) != sizeof(styleWire)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    CssStyle style;
    if (!decodeStyleWire(styleWire, style)) {
      clear();
      return CacheLoadResult::Invalid;
    }

    KeyPieces key;
    key.add(std::string_view(selectorBuffer.get(), selectorLen));
    const RuleInsertResult insertResult = insertOrMerge(key, style);
    if (insertResult == RuleInsertResult::OutOfMemory) {
      clear();
      return CacheLoadResult::LowMemory;
    }
    if (insertResult == RuleInsertResult::Limit) {
      clear();
      return CacheLoadResult::Invalid;
    }
    if (insertResult == RuleInsertResult::Merged) {
      LOG_DBG("CSS", "Duplicate selector in CSS cache");
      clear();
      return CacheLoadResult::Invalid;
    }
  }

  if (file.available() != 0) {
    clear();
    return CacheLoadResult::Invalid;
  }

  const bool partial = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;
  LOG_DBG("CSS", "Loaded %u rules from %s cache", ruleCount, partial ? "partial" : "complete");
  return CacheLoadResult::Complete;
}
