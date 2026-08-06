#include "CssParser.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
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

// Maximum number of CSS rules to store in the selector index
// Prevents unbounded memory growth from pathological CSS files.
// At ~22 bytes per rule (8-byte index entry + pooled selector string) the
// cap costs ~45KB; the old per-node map spent ~144 bytes per rule.
constexpr size_t MAX_RULES = 2048;

// Maximum total bytes of pooled selector text
constexpr size_t STRING_POOL_CAP = 32 * 1024;

// Maximum number of deduplicated style bodies. Per-chapter-converted books
// re-declare the same few styles under fresh class names, so real books need
// far fewer unique bodies than rules (issue #2591's book: 1255 rules, 83
// bodies). SelectorEntry::styleIdx is u16, so this can be raised without a
// cache format change.
constexpr size_t MAX_UNIQUE_STYLES = 256;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

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

// FNV-1a per Fowler/Noll/Vo, sized to match size_t on the target. The firmware
// runs on a 32-bit core where size_t is 32 bits, so naively using the 64-bit
// constants would silently truncate FNV_PRIME to a non-prime and wreck hash
// distribution. The selection below picks the canonical 32- or 64-bit
// constants at compile time so the same source works in a 64-bit host
// simulator. `fnv1aMix` is the per-byte mix step; callers apply any
// byte-level transform (e.g. asciiToLower) first.
static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV constants are only defined for 32- or 64-bit size_t");
constexpr size_t FNV_OFFSET_BASIS =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME =
    sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

constexpr size_t fnv1aMix(size_t hash, unsigned char byte) { return (hash ^ byte) * FNV_PRIME; }

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

// Canonical fixed-size wire encoding of a CssStyle: 5 enum bytes, 11 CssLength
// records (float value + unit byte), display, verticalAlign, then the
// defined-property bits as u32. Shared by the style dedup comparison and the
// cache format so "same style" and "same cache record" are one definition.
// CssStyle itself has padding and a bitfield, so raw memcmp of the struct is
// not a valid equality test — the wire bytes are. Multi-byte fields go through
// memcpy (RISC-V faults on unaligned access; both targets are little-endian).
constexpr size_t STYLE_WIRE_BYTES = 5 + 11 * (sizeof(float) + 1) + 2 + sizeof(uint32_t);

void encodeStyleWire(const CssStyle& style, uint8_t out[STYLE_WIRE_BYTES]) {
  size_t o = 0;
  out[o++] = static_cast<uint8_t>(style.textAlign);
  out[o++] = static_cast<uint8_t>(style.fontStyle);
  out[o++] = static_cast<uint8_t>(style.fontWeight);
  out[o++] = static_cast<uint8_t>(style.textDecoration);
  out[o++] = static_cast<uint8_t>(style.direction);

  auto putLength = [&](const CssLength& len) {
    memcpy(out + o, &len.value, sizeof(len.value));
    o += sizeof(len.value);
    out[o++] = static_cast<uint8_t>(len.unit);
  };
  putLength(style.textIndent);
  putLength(style.marginTop);
  putLength(style.marginBottom);
  putLength(style.marginLeft);
  putLength(style.marginRight);
  putLength(style.paddingTop);
  putLength(style.paddingBottom);
  putLength(style.paddingLeft);
  putLength(style.paddingRight);
  putLength(style.imageHeight);
  putLength(style.imageWidth);

  out[o++] = static_cast<uint8_t>(style.display);
  out[o++] = static_cast<uint8_t>(style.verticalAlign);

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
  memcpy(out + o, &definedBits, sizeof(definedBits));
}

void decodeStyleWire(const uint8_t in[STYLE_WIRE_BYTES], CssStyle& style) {
  size_t o = 0;
  style.textAlign = static_cast<CssTextAlign>(in[o++]);
  style.fontStyle = static_cast<CssFontStyle>(in[o++]);
  style.fontWeight = static_cast<CssFontWeight>(in[o++]);
  style.textDecoration = static_cast<CssTextDecoration>(in[o++] & CSS_TEXT_DECORATION_MASK);
  style.direction = static_cast<CssTextDirection>(in[o++]);

  auto getLength = [&](CssLength& len) {
    memcpy(&len.value, in + o, sizeof(len.value));
    o += sizeof(len.value);
    len.unit = static_cast<CssUnit>(in[o++]);
  };
  getLength(style.textIndent);
  getLength(style.marginTop);
  getLength(style.marginBottom);
  getLength(style.marginLeft);
  getLength(style.marginRight);
  getLength(style.paddingTop);
  getLength(style.paddingBottom);
  getLength(style.paddingLeft);
  getLength(style.paddingRight);
  getLength(style.imageHeight);
  getLength(style.imageWidth);

  style.display = static_cast<CssDisplay>(in[o++]);
  style.verticalAlign = static_cast<CssVerticalAlign>(in[o++]);

  uint32_t definedBits = 0;
  memcpy(&definedBits, in + o, sizeof(definedBits));
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
}

uint32_t hashStyleWire(const uint8_t (&wire)[STYLE_WIRE_BYTES]) {
  size_t h = FNV_OFFSET_BASIS;
  for (const uint8_t b : wire) h = fnv1aMix(h, b);
  return static_cast<uint32_t>(h);
}

}  // anonymous namespace

// Sorted-index primitives. Stored selectors are case-folded at insert, so a
// stored-vs-probe comparison folds only the probe side, and stored-vs-stored
// order (used by the cache sortedness check) is plain unsigned byte order.

int CssParser::compareEntryToPieces(const SelectorEntry& entry, const std::string_view p0, const std::string_view p1,
                                    const std::string_view p2) const {
  // Compares the stored folded selector against the case-folded virtual
  // concatenation p0+p1+p2 without materializing it. Returns <0/0/>0.
  const char* stored = selectorPool_.get() + entry.offset;
  const size_t storedLen = entry.length;
  const std::string_view pieces[3] = {p0, p1, p2};
  size_t i = 0;
  for (const std::string_view piece : pieces) {
    for (const char c : piece) {
      if (i == storedLen) return -1;  // stored is a proper prefix of the probe
      const auto a = static_cast<unsigned char>(stored[i]);
      const auto b = static_cast<unsigned char>(asciiToLower(c));
      if (a != b) return a < b ? -1 : 1;
      ++i;
    }
  }
  return i == storedLen ? 0 : 1;  // probe is a proper prefix of stored
}

size_t CssParser::lowerBound(const std::string_view p0, const std::string_view p1, const std::string_view p2,
                             bool& exact) const {
  size_t lo = 0;
  size_t hi = entryCount_;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (compareEntryToPieces(entries_[mid], p0, p1, p2) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  exact = lo < entryCount_ && compareEntryToPieces(entries_[lo], p0, p1, p2) == 0;
  return lo;
}

const CssStyle* CssParser::findStyle(const std::string_view p0, const std::string_view p1,
                                     const std::string_view p2) const {
  bool exact = false;
  const size_t pos = lowerBound(p0, p1, p2, exact);
  return exact ? &stylePool_[entries_[pos].styleIdx] : nullptr;
}

// Pool builders. Growth is geometric via nothrow allocation; on OOM or cap the
// caller skips the rule (styles degrade, firmware never aborts).

bool CssParser::ensureEntryCapacity(const size_t needed) {
  if (needed <= entryCapacity_) return true;
  size_t newCap = entryCapacity_ ? entryCapacity_ * 2u : 256u;
  while (newCap < needed) newCap *= 2;
  if (newCap > MAX_RULES) newCap = MAX_RULES;
  if (needed > newCap) return false;
  auto grown = makeUniqueNoThrow<SelectorEntry[]>(newCap);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector index (%zu entries)", newCap);
    return false;
  }
  if (entryCount_ > 0) memcpy(grown.get(), entries_.get(), entryCount_ * sizeof(SelectorEntry));
  entries_ = std::move(grown);
  entryCapacity_ = static_cast<uint16_t>(newCap);
  return true;
}

bool CssParser::ensurePoolCapacity(const size_t needed) {
  if (needed <= poolCapacity_) return true;
  if (needed > STRING_POOL_CAP) {
    LOG_DBG("CSS", "Selector pool full (%zu > %zu), skipping", needed, STRING_POOL_CAP);
    return false;
  }
  size_t newCap = poolCapacity_ ? poolCapacity_ * 2u : 4096u;
  while (newCap < needed) newCap *= 2;
  if (newCap > STRING_POOL_CAP) newCap = STRING_POOL_CAP;
  auto grown = makeUniqueNoThrow<char[]>(newCap);
  if (!grown) {
    LOG_ERR("CSS", "OOM: selector pool (%zu bytes)", newCap);
    return false;
  }
  if (poolSize_ > 0) memcpy(grown.get(), selectorPool_.get(), poolSize_);
  selectorPool_ = std::move(grown);
  poolCapacity_ = static_cast<uint32_t>(newCap);
  return true;
}

bool CssParser::ensureStyleCapacity(const size_t needed) {
  if (needed <= styleCapacity_) return true;
  if (needed > MAX_UNIQUE_STYLES) {
    LOG_DBG("CSS", "Unique style limit reached (%zu), skipping", MAX_UNIQUE_STYLES);
    return false;
  }
  size_t newCap = styleCapacity_ ? styleCapacity_ * 2u : 32u;
  while (newCap < needed) newCap *= 2;
  if (newCap > MAX_UNIQUE_STYLES) newCap = MAX_UNIQUE_STYLES;
  auto grownStyles = makeUniqueNoThrow<CssStyle[]>(newCap);
  auto grownHashes = makeUniqueNoThrow<uint32_t[]>(newCap);
  if (!grownStyles || !grownHashes) {
    LOG_ERR("CSS", "OOM: style pool (%zu styles)", newCap);
    return false;
  }
  for (size_t i = 0; i < styleCount_; ++i) grownStyles[i] = stylePool_[i];
  if (styleCount_ > 0) memcpy(grownHashes.get(), styleHashes_.get(), styleCount_ * sizeof(uint32_t));
  stylePool_ = std::move(grownStyles);
  styleHashes_ = std::move(grownHashes);
  styleCapacity_ = static_cast<uint16_t>(newCap);
  return true;
}

bool CssParser::internStyle(const CssStyle& style, uint16_t& idxOut) {
  uint8_t wire[STYLE_WIRE_BYTES];
  encodeStyleWire(style, wire);
  const uint32_t hash = hashStyleWire(wire);
  for (uint16_t i = 0; i < styleCount_; ++i) {
    if (styleHashes_[i] != hash) continue;
    uint8_t existing[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[i], existing);
    if (memcmp(existing, wire, STYLE_WIRE_BYTES) == 0) {
      idxOut = i;
      return true;
    }
  }
  if (!ensureStyleCapacity(static_cast<size_t>(styleCount_) + 1)) return false;
  stylePool_[styleCount_] = style;
  styleHashes_[styleCount_] = hash;
  idxOut = styleCount_;
  ++styleCount_;
  return true;
}

bool CssParser::appendSelector(const std::string_view sel, uint32_t& offsetOut) {
  if (!ensurePoolCapacity(static_cast<size_t>(poolSize_) + sel.size())) return false;
  offsetOut = poolSize_;
  char* dst = selectorPool_.get() + poolSize_;
  for (const char c : sel) *dst++ = asciiToLower(c);
  poolSize_ += static_cast<uint32_t>(sel.size());
  return true;
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

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  const std::string_view value = trimCssWhitespace(decl.substr(colonPos + 1));

  if (name.empty() || value.empty()) return;

  if (iequalsAscii(name, "text-align")) {
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
    const std::string_view displayValue = stripTrailingImportant(value);
    style.display = iequalsAscii(displayValue, "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "direction")) {
    const std::string_view directionValue = stripTrailingImportant(value);
    if (iequalsAscii(directionValue, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(directionValue, "ltr")) {
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

  // Check if we've reached the rule limit before processing
  if (entryCount_ >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently.
  bool limitReached = false;
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // TODO: Support richer CSS selector syntax in the future. For now we only
        // handle `tag`, `.class`, or `tag.class`. Reject anything containing a
        // character that introduces unsupported syntax:
        //   '+'  adjacent sibling combinator
        //   '>'  child combinator
        //   '['  attribute selector
        //   ':'  pseudo class/element
        //   '#'  ID selector
        //   '~'  general sibling combinator
        //   '*'  wildcard
        //   ' '  descendant combinator
        // Single-pass scan via find_first_of instead of eight sequential find() calls.
        constexpr std::string_view kUnsupportedSelectorChars = "+>[:#~* ";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;

        // Selectors are stored case-folded, so two selectors that differ only
        // in ASCII case land on the same entry and merge.
        bool exact = false;
        const size_t pos = lowerBound(sel, {}, {}, exact);
        if (exact) {
          // Duplicate selector: later declarations win property-by-property.
          // On intern failure keep the pre-merge style — degrade, never corrupt.
          SelectorEntry& entry = entries_[pos];
          CssStyle merged = stylePool_[entry.styleIdx];
          merged.applyOver(style);
          uint16_t mergedIdx = 0;
          if (internStyle(merged, mergedIdx)) entry.styleIdx = mergedIdx;
          return;
        }

        // Skip if this would exceed the rule limit
        if (entryCount_ >= MAX_RULES) {
          LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
          limitReached = true;
          return;
        }

        // Ordered so a failure partway leaves at worst an orphaned pooled
        // style, never a dangling index entry.
        if (!ensureEntryCapacity(static_cast<size_t>(entryCount_) + 1)) return;
        uint16_t styleIdx = 0;
        if (!internStyle(style, styleIdx)) return;
        uint32_t offset = 0;
        if (!appendSelector(sel, offset)) return;

        SelectorEntry* base = entries_.get();
        memmove(base + pos + 1, base + pos, (entryCount_ - pos) * sizeof(SelectorEntry));
        base[pos] = SelectorEntry{offset, styleIdx, static_cast<uint16_t>(sel.size())};
        ++entryCount_;
      });
}

// Main parsing entry point

bool CssParser::loadFromStream(HalFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
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
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
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
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector, currentStyle);
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
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer, currentStyle);
          declBuffer.clear();
        }
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

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", ruleCount(), totalRead);
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr) const {
  CssStyle result;
  if (entryCount_ == 0) return result;

  // 1. Apply element-level style (lowest priority). Lookups fold case, so the
  // raw tagName view can be used as the probe.
  if (const CssStyle* style = findStyle(tagName)) {
    result.applyOver(*style);
  }

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority). The probe pieces are compared as
  // if concatenated, so we never materialize ".classname".
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (const CssStyle* style = findStyle(".", cls)) {
      result.applyOver(*style);
    }
  });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (const CssStyle* style = findStyle(tagName, ".", cls)) {
      result.applyOver(*style);
    }
  });

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

// Cache format v8:
//   u8  version
//   u16 entryCount E, u16 styleCount S, u32 stringPoolBytes P
//   E*8  SelectorEntry records (persisted sorted — load never sorts)
//   S*66 style wire records (encodeStyleWire layout)
//   P    case-folded selector chars, no separators
// File size must equal 9 + 8E + 66S + P exactly.

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Header fields are memcpy'd into place: both targets are little-endian and
  // RISC-V faults on unaligned stores through cast pointers.
  uint8_t header[9];
  header[0] = CssParser::CSS_CACHE_VERSION;
  memcpy(header + 1, &entryCount_, sizeof(entryCount_));
  memcpy(header + 3, &styleCount_, sizeof(styleCount_));
  memcpy(header + 5, &poolSize_, sizeof(poolSize_));
  file.write(header, sizeof(header));

  if (entryCount_ > 0) {
    file.write(entries_.get(), entryCount_ * sizeof(SelectorEntry));
  }

  for (uint16_t i = 0; i < styleCount_; ++i) {
    uint8_t wire[STYLE_WIRE_BYTES];
    encodeStyleWire(stylePool_[i], wire);
    file.write(wire, STYLE_WIRE_BYTES);
  }

  if (poolSize_ > 0) {
    file.write(selectorPool_.get(), poolSize_);
  }

  LOG_DBG("CSS", "Saved %u rules to cache", entryCount_);
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
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
    return false;
  }

  uint8_t header[8];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  uint16_t entryCount = 0;
  uint16_t styleCount = 0;
  uint32_t poolBytes = 0;
  memcpy(&entryCount, header, sizeof(entryCount));
  memcpy(&styleCount, header + 2, sizeof(styleCount));
  memcpy(&poolBytes, header + 4, sizeof(poolBytes));

  if (entryCount > MAX_RULES || styleCount > MAX_UNIQUE_STYLES || poolBytes > STRING_POOL_CAP) {
    LOG_DBG("CSS", "Invalid cache header (%u rules, %u styles, %u pool bytes)", entryCount, styleCount,
            static_cast<unsigned>(poolBytes));
    return false;
  }

  // The remaining payload size must match the header exactly
  const size_t expectedBytes = entryCount * sizeof(SelectorEntry) + styleCount * STYLE_WIRE_BYTES + poolBytes;
  if (static_cast<size_t>(file.available()) != expectedBytes) {
    LOG_DBG("CSS", "CSS cache size mismatch");
    return false;
  }

  if (entryCount > 0) {
    entries_ = makeUniqueNoThrow<SelectorEntry[]>(entryCount);
    if (!entries_) {
      LOG_ERR("CSS", "OOM: selector index (%u entries)", entryCount);
      clear();
      return false;
    }
    const size_t entryBytes = entryCount * sizeof(SelectorEntry);
    if (file.read(entries_.get(), entryBytes) != static_cast<int>(entryBytes)) {
      clear();
      return false;
    }
  }

  if (styleCount > 0) {
    stylePool_ = makeUniqueNoThrow<CssStyle[]>(styleCount);
    styleHashes_ = makeUniqueNoThrow<uint32_t[]>(styleCount);
    if (!stylePool_ || !styleHashes_) {
      LOG_ERR("CSS", "OOM: style pool (%u styles)", styleCount);
      clear();
      return false;
    }
    for (uint16_t i = 0; i < styleCount; ++i) {
      uint8_t wire[STYLE_WIRE_BYTES];
      if (file.read(wire, STYLE_WIRE_BYTES) != static_cast<int>(STYLE_WIRE_BYTES)) {
        clear();
        return false;
      }
      decodeStyleWire(wire, stylePool_[i]);
      styleHashes_[i] = hashStyleWire(wire);
    }
  }

  if (poolBytes > 0) {
    selectorPool_ = makeUniqueNoThrow<char[]>(poolBytes);
    if (!selectorPool_) {
      LOG_ERR("CSS", "OOM: selector pool (%u bytes)", static_cast<unsigned>(poolBytes));
      clear();
      return false;
    }
    if (file.read(selectorPool_.get(), poolBytes) != static_cast<int>(poolBytes)) {
      clear();
      return false;
    }
  }

  entryCount_ = entryCount;
  entryCapacity_ = entryCount;
  styleCount_ = styleCount;
  styleCapacity_ = styleCount;
  poolSize_ = poolBytes;
  poolCapacity_ = poolBytes;

  // Validate every entry so a corrupt cache cannot break binary search or
  // index out of bounds: offsets/lengths in range, style indices valid, and
  // entries strictly ascending in folded-byte order.
  const char* pool = selectorPool_.get();
  for (uint16_t i = 0; i < entryCount; ++i) {
    const SelectorEntry& entry = entries_[i];
    if (entry.length == 0 || entry.length > MAX_SELECTOR_LENGTH || entry.offset > poolBytes ||
        static_cast<size_t>(entry.offset) + entry.length > poolBytes || entry.styleIdx >= styleCount) {
      LOG_DBG("CSS", "Invalid cache entry %u", i);
      clear();
      return false;
    }
    if (i > 0) {
      const SelectorEntry& prev = entries_[i - 1];
      const uint16_t commonLen = prev.length < entry.length ? prev.length : entry.length;
      const int cmp = memcmp(pool + prev.offset, pool + entry.offset, commonLen);
      if (cmp > 0 || (cmp == 0 && prev.length >= entry.length)) {
        LOG_DBG("CSS", "Cache entries not sorted at %u", i);
        clear();
        return false;
      }
    }
  }

  LOG_DBG("CSS", "Loaded %u rules from cache", entryCount);
  return true;
}
