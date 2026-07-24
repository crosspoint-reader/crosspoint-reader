#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>

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

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Full mode intentionally supports only a bounded, useful subset of descendant
// selectors. This keeps malformed publisher CSS from multiplying lookup work.
constexpr size_t MAX_DESCENDANT_RULES = 100;
constexpr size_t MAX_CLASSES_PER_ELEMENT = 4;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

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

}  // anonymous namespace

// Transparent case-insensitive hash/equal. Bodies live here (rather than
// inline in the header) so they can share the anonymous-namespace asciiToLower
// with the other ASCII helpers in this translation unit.

size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept {
  size_t h = FNV_OFFSET_BASIS;
  for (char c : sv) h = fnv1aMix(h, asciiToLower(c));
  return h;
}

size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

size_t CssParser::SvHash::operator()(CompositeKey k) const noexcept {
  // Hash the case-folded concatenation of every piece without materializing
  // it — the running hash continues across pieces as if they were one buffer.
  size_t h = FNV_OFFSET_BASIS;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) h = fnv1aMix(h, asciiToLower(c));
  }
  return h;
}

bool CssParser::SvEqual::operator()(std::string_view a, std::string_view b) const noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
  }
  return true;
}

bool CssParser::SvEqual::operator()(const std::string& a, std::string_view b) const noexcept {
  return operator()(std::string_view(a), b);
}

bool CssParser::SvEqual::operator()(std::string_view a, const std::string& b) const noexcept {
  return operator()(a, std::string_view(b));
}

bool CssParser::SvEqual::operator()(const std::string& a, const std::string& b) const noexcept {
  return operator()(std::string_view(a), std::string_view(b));
}

bool CssParser::SvEqual::operator()(CompositeKey k, std::string_view sv) const noexcept {
  size_t total = 0;
  for (std::string_view piece : k.pieces) total += piece.size();
  if (total != sv.size()) return false;
  size_t i = 0;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) {
      if (asciiToLower(c) != asciiToLower(sv[i++])) return false;
    }
  }
  return true;
}

bool CssParser::SvEqual::operator()(std::string_view sv, CompositeKey k) const noexcept { return operator()(k, sv); }

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
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently; the only heap allocation per kept selector is the std::string
  // map key, which is unavoidable since the map owns its keys.
  bool limitReached = false;
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // Support `tag`, `.class`, `tag.class`, plus one descendant combinator.
        // Reject anything containing syntax outside that deliberately small set:
        //   '+'  adjacent sibling combinator
        //   '>'  child combinator
        //   '['  attribute selector
        //   ':'  pseudo class/element
        //   '#'  ID selector
        //   '~'  general sibling combinator
        //   '*'  wildcard
        // Single-pass scan via find_first_of instead of sequential find() calls.
        constexpr std::string_view kUnsupportedSelectorChars = "+>[:#~*";
        if (sel.find_first_of(kUnsupportedSelectorChars) != std::string_view::npos) return;

        std::string_view selectorParts[2];
        size_t selectorPartCount = 0;
        bool tooManyParts = false;
        forEachDelimitedToken(sel, isCssWhitespace, [&](const std::string_view part) {
          if (selectorPartCount < std::size(selectorParts)) {
            selectorParts[selectorPartCount++] = part;
          } else {
            tooManyParts = true;
          }
        });
        if (tooManyParts || selectorPartCount == 0) return;

        const auto isSimpleSelector = [](const std::string_view part) {
          return !part.empty() && part.find_first_of("+>[:#~* \t\r\n\f") == std::string_view::npos;
        };
        if (!isSimpleSelector(selectorParts[0]) ||
            (selectorPartCount == 2 && !isSimpleSelector(selectorParts[1]))) {
          return;
        }

        if (selectorPartCount == 2) {
          size_t descendantCount = 0;
          for (const auto& pair : rulesBySelector_) {
            if (pair.first.find(' ') != std::string::npos) ++descendantCount;
          }
          if (descendantCount >= MAX_DESCENDANT_RULES) return;
        }

        // Skip if this would exceed the rule limit
        if (rulesBySelector_.size() >= MAX_RULES) {
          LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
          limitReached = true;
          return;
        }

        // Store or merge with existing. Hash/equal are case-insensitive, so two
        // selectors that differ only in ASCII case collide on insert and merge.
        std::string normalizedSelector(selectorParts[0]);
        if (selectorPartCount == 2) {
          normalizedSelector.push_back(' ');
          normalizedSelector.append(selectorParts[1]);
        }
        auto it = rulesBySelector_.find(normalizedSelector);
        if (it != rulesBySelector_.end()) {
          it->second.applyOver(style);
        } else {
          rulesBySelector_.emplace(std::move(normalizedSelector), style);
        }
      });
}

// Main parsing entry point

bool CssParser::loadFromStream(HalFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  const size_t expectedSize = source.fileSize();
  size_t totalRead = 0;
  bool readFailed = false;

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
    if (bytesRead <= 0) {
      readFailed = true;
      break;
    }

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

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  if (readFailed || totalRead != expectedSize) {
    LOG_ERR("CSS", "CSS read stopped early: %zu/%zu bytes", totalRead, expectedSize);
    return false;
  }
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr) const {
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

  // 1. Apply element-level style (lowest priority). The map's hash/equal are
  // case-insensitive, so the raw tagName view can be used as the lookup key.
  if (auto it = rulesBySelector_.find(tagName); it != rulesBySelector_.end()) {
    result.applyOver(it->second);
  }

  if (classAttr.empty()) return result;

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority). The transparent hash/equal accept
  // a CompositeKey, so we never materialize the concatenation.
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (auto it = rulesBySelector_.find(CompositeKey{".", cls}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
  });

  // TODO: Support combinations of classes (e.g. style on p.class1.class2)
  // 3. Apply element.class styles (higher priority).
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (auto it = rulesBySelector_.find(CompositeKey{tagName, ".", cls}); it != rulesBySelector_.end()) {
      result.applyOver(it->second);
    }
  });

  return result;
}

CssStyle CssParser::resolveStyle(std::string_view tagName, std::string_view classAttr,
                                 const std::vector<AncestorEntry>& ancestors) const {
  CssStyle result = resolveStyle(tagName, classAttr);
  if (ancestors.empty()) return result;

  const auto forEachClass = [](const std::string_view classes, const auto& fn) {
    size_t count = 0;
    forEachDelimitedToken(classes, isCssWhitespace, [&](const std::string_view cls) {
      if (count++ < MAX_CLASSES_PER_ELEMENT) fn(cls);
    });
  };
  const auto applyRule = [this, &result](const CompositeKey key) {
    if (const auto it = rulesBySelector_.find(key); it != rulesBySelector_.end()) result.applyOver(it->second);
  };
  const auto applyChildVariants = [&](const std::string_view parent0, const std::string_view parent1,
                                      const std::string_view parent2) {
    applyRule(CompositeKey{parent0, parent1, parent2, " ", tagName});
    forEachClass(classAttr, [&](const std::string_view childClass) {
      applyRule(CompositeKey{parent0, parent1, parent2, " ", ".", childClass});
      applyRule(CompositeKey{parent0, parent1, parent2, " ", tagName, ".", childClass});
    });
  };

  // Descendant rules sit between the basic element rule and the current
  // element's class-specific rules. Re-applying the basic rules afterwards
  // preserves the existing CrossVi priority model for the target element.
  for (const AncestorEntry& ancestor : ancestors) {
    if (ancestor.tag.empty()) continue;
    applyChildVariants(ancestor.tag, {}, {});
    forEachClass(ancestor.classAttr, [&](const std::string_view ancestorClass) {
      applyChildVariants(".", ancestorClass, {});
      applyChildVariants(ancestor.tag, ".", ancestorClass);
    });
  }
  const CssStyle targetRules = resolveStyle(tagName, classAttr);
  result.applyOver(targetRules);
  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

namespace {

// Cache file names (version is CssParser::CSS_CACHE_VERSION). The temporary
// file lives beside the canonical file so publication is a same-filesystem
// rename after all bytes have been synchronized.
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char rulesCacheTemp[] = "/css_rules.cache.tmp";
constexpr uint32_t CSS_DEFINED_BITS_MASK = (1U << 18U) - 1U;

uint32_t updateCacheCrc(uint32_t crc, const void* data, const size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc;
}

}  // namespace

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  const std::string temporaryPath = cachePath + rulesCacheTemp;
  if (Storage.exists(temporaryPath.c_str())) Storage.remove(temporaryPath.c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty() || rulesBySelector_.size() > MAX_RULES) return false;

  const std::string canonicalPath = cachePath + rulesCache;
  const std::string temporaryPath = cachePath + rulesCacheTemp;
  if (Storage.exists(temporaryPath.c_str()) &&
      (!Storage.remove(temporaryPath.c_str()) || Storage.exists(temporaryPath.c_str()))) {
    return false;
  }

  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  bool fileComplete = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("CSS", temporaryPath, file)) return false;

    bool writeOk = true;
    uint32_t crc = UINT32_MAX;
    const auto writePayload = [&file, &writeOk, &crc](const void* data, const size_t size) {
      if (!writeOk || file.write(data, size) != size) {
        writeOk = false;
        return false;
      }
      crc = updateCacheCrc(crc, data, size);
      return true;
    };
    const auto writeByte = [&writePayload](const uint8_t value) { return writePayload(&value, sizeof(value)); };

    writeByte(CssParser::CSS_CACHE_VERSION);
    writePayload(&ruleCount, sizeof(ruleCount));

    for (const auto& pair : rulesBySelector_) {
      if (!writeOk || pair.first.empty() || pair.first.size() > MAX_SELECTOR_LENGTH) {
        writeOk = false;
        break;
      }

      const auto selectorLen = static_cast<uint16_t>(pair.first.size());
      writePayload(&selectorLen, sizeof(selectorLen));
      writePayload(pair.first.data(), selectorLen);

      const CssStyle& style = pair.second;
      writeByte(static_cast<uint8_t>(style.textAlign));
      writeByte(static_cast<uint8_t>(style.fontStyle));
      writeByte(static_cast<uint8_t>(style.fontWeight));
      writeByte(static_cast<uint8_t>(style.textDecoration));
      writeByte(static_cast<uint8_t>(style.direction));

      const auto writeLength = [&writePayload, &writeByte](const CssLength& len) {
        return writePayload(&len.value, sizeof(len.value)) && writeByte(static_cast<uint8_t>(len.unit));
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
      writeByte(static_cast<uint8_t>(style.display));
      writeByte(static_cast<uint8_t>(style.verticalAlign));

      uint32_t definedBits = 0;
      if (style.defined.textAlign) definedBits |= 1U << 0U;
      if (style.defined.fontStyle) definedBits |= 1U << 1U;
      if (style.defined.fontWeight) definedBits |= 1U << 2U;
      if (style.defined.textDecoration) definedBits |= 1U << 3U;
      if (style.defined.textIndent) definedBits |= 1U << 4U;
      if (style.defined.marginTop) definedBits |= 1U << 5U;
      if (style.defined.marginBottom) definedBits |= 1U << 6U;
      if (style.defined.marginLeft) definedBits |= 1U << 7U;
      if (style.defined.marginRight) definedBits |= 1U << 8U;
      if (style.defined.paddingTop) definedBits |= 1U << 9U;
      if (style.defined.paddingBottom) definedBits |= 1U << 10U;
      if (style.defined.paddingLeft) definedBits |= 1U << 11U;
      if (style.defined.paddingRight) definedBits |= 1U << 12U;
      if (style.defined.imageHeight) definedBits |= 1U << 13U;
      if (style.defined.imageWidth) definedBits |= 1U << 14U;
      if (style.defined.display) definedBits |= 1U << 15U;
      if (style.defined.direction) definedBits |= 1U << 16U;
      if (style.defined.verticalAlign) definedBits |= 1U << 17U;
      writePayload(&definedBits, sizeof(definedBits));
    }

    const uint32_t storedCrc = ~crc;
    if (writeOk && file.write(&storedCrc, sizeof(storedCrc)) != sizeof(storedCrc)) writeOk = false;
    if (writeOk) file.flush();
    const bool synced = writeOk && file.sync();
    const bool closed = file.close();
    fileComplete = writeOk && synced && closed;
  }

  if (!fileComplete) {
    if (Storage.exists(temporaryPath.c_str())) Storage.remove(temporaryPath.c_str());
    return false;
  }

  if (Storage.exists(canonicalPath.c_str()) &&
      (!Storage.remove(canonicalPath.c_str()) || Storage.exists(canonicalPath.c_str()))) {
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (!Storage.rename(temporaryPath.c_str(), canonicalPath.c_str())) {
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) return false;

  const std::string canonicalPath = cachePath + rulesCache;
  HalFile file;
  if (!Storage.openFileForRead("CSS", canonicalPath, file)) return false;

  clear();
  uint16_t decodedRuleCount = 0;
  uint32_t crc = UINT32_MAX;
  const auto readPayload = [&file, &crc](void* data, const size_t size) {
    const int available = file.available();
    if (available < 0 || static_cast<size_t>(available) < size || file.read(data, size) != static_cast<int>(size)) {
      return false;
    }
    crc = updateCacheCrc(crc, data, size);
    return true;
  };
  const auto decode = [&]() -> bool {
    uint8_t version = 0;
    if (!readPayload(&version, sizeof(version)) || version != CssParser::CSS_CACHE_VERSION) {
      LOG_DBG("CSS", "CSS cache version mismatch or unreadable");
      return false;
    }
    if (!readPayload(&decodedRuleCount, sizeof(decodedRuleCount)) || decodedRuleCount > MAX_RULES) return false;

    rulesBySelector_.reserve(decodedRuleCount);
    constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
    constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
    constexpr size_t CSS_FIXED_STYLE_BYTES =
        5 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) + 2 * sizeof(uint8_t) + sizeof(uint32_t);

    for (uint16_t i = 0; i < decodedRuleCount; ++i) {
      uint16_t selectorLen = 0;
      if (!readPayload(&selectorLen, sizeof(selectorLen)) || selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH) {
        return false;
      }
      const int selectorBytesAvailable = file.available();
      if (selectorBytesAvailable < 0 || static_cast<size_t>(selectorBytesAvailable) < selectorLen) return false;

      std::string selector(selectorLen, '\0');
      if (!readPayload(selector.data(), selector.size())) return false;
      const int styleBytesAvailable = file.available();
      if (styleBytesAvailable < 0 ||
          static_cast<size_t>(styleBytesAvailable) < CSS_FIXED_STYLE_BYTES + sizeof(uint32_t)) {
        return false;
      }

      CssStyle style;
      uint8_t textAlign = 0;
      uint8_t fontStyle = 0;
      uint8_t fontWeight = 0;
      uint8_t textDecoration = 0;
      uint8_t direction = 0;
      if (!readPayload(&textAlign, 1) || !readPayload(&fontStyle, 1) || !readPayload(&fontWeight, 1) ||
          !readPayload(&textDecoration, 1) || !readPayload(&direction, 1) ||
          textAlign > static_cast<uint8_t>(CssTextAlign::None) ||
          fontStyle > static_cast<uint8_t>(CssFontStyle::Italic) ||
          fontWeight > static_cast<uint8_t>(CssFontWeight::Bold) || (textDecoration & ~CSS_TEXT_DECORATION_MASK) != 0 ||
          direction > static_cast<uint8_t>(CssTextDirection::Rtl)) {
        return false;
      }
      style.textAlign = static_cast<CssTextAlign>(textAlign);
      style.fontStyle = static_cast<CssFontStyle>(fontStyle);
      style.fontWeight = static_cast<CssFontWeight>(fontWeight);
      style.textDecoration = static_cast<CssTextDecoration>(textDecoration);
      style.direction = static_cast<CssTextDirection>(direction);

      const auto readLength = [&readPayload](CssLength& length) {
        uint8_t unit = 0;
        if (!readPayload(&length.value, sizeof(length.value)) || !readPayload(&unit, sizeof(unit)) ||
            !std::isfinite(length.value) || unit > static_cast<uint8_t>(CssUnit::Percent)) {
          return false;
        }
        length.unit = static_cast<CssUnit>(unit);
        return true;
      };
      if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
          !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
          !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
          !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
        return false;
      }

      uint8_t display = 0;
      uint8_t verticalAlign = 0;
      uint32_t definedBits = 0;
      if (!readPayload(&display, sizeof(display)) || !readPayload(&verticalAlign, sizeof(verticalAlign)) ||
          !readPayload(&definedBits, sizeof(definedBits)) || display > static_cast<uint8_t>(CssDisplay::None) ||
          verticalAlign > static_cast<uint8_t>(CssVerticalAlign::Sub) || (definedBits & ~CSS_DEFINED_BITS_MASK) != 0) {
        return false;
      }
      style.display = static_cast<CssDisplay>(display);
      style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlign);
      style.defined.textAlign = (definedBits & 1U << 0U) != 0;
      style.defined.fontStyle = (definedBits & 1U << 1U) != 0;
      style.defined.fontWeight = (definedBits & 1U << 2U) != 0;
      style.defined.textDecoration = (definedBits & 1U << 3U) != 0;
      style.defined.textIndent = (definedBits & 1U << 4U) != 0;
      style.defined.marginTop = (definedBits & 1U << 5U) != 0;
      style.defined.marginBottom = (definedBits & 1U << 6U) != 0;
      style.defined.marginLeft = (definedBits & 1U << 7U) != 0;
      style.defined.marginRight = (definedBits & 1U << 8U) != 0;
      style.defined.paddingTop = (definedBits & 1U << 9U) != 0;
      style.defined.paddingBottom = (definedBits & 1U << 10U) != 0;
      style.defined.paddingLeft = (definedBits & 1U << 11U) != 0;
      style.defined.paddingRight = (definedBits & 1U << 12U) != 0;
      style.defined.imageHeight = (definedBits & 1U << 13U) != 0;
      style.defined.imageWidth = (definedBits & 1U << 14U) != 0;
      style.defined.display = (definedBits & 1U << 15U) != 0;
      style.defined.direction = (definedBits & 1U << 16U) != 0;
      style.defined.verticalAlign = (definedBits & 1U << 17U) != 0;

      if (!rulesBySelector_.emplace(std::move(selector), style).second) return false;
    }

    if (file.available() != static_cast<int>(sizeof(uint32_t))) return false;
    uint32_t storedCrc = 0;
    if (file.read(&storedCrc, sizeof(storedCrc)) != sizeof(storedCrc) || file.available() != 0) return false;
    return storedCrc == ~crc;
  };

  const bool decoded = decode();
  const bool closed = file.close();
  if (!decoded || !closed) {
    clear();
    if (Storage.exists(canonicalPath.c_str())) Storage.remove(canonicalPath.c_str());
    return false;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache", decodedRuleCount);
  return true;
}
