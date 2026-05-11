#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
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

  // Convert to string for passing to functions (single allocation)
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
constexpr size_t CSS_FIXED_STYLE_BYTES =
    4 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) + sizeof(uint8_t) + sizeof(uint16_t);

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char compileTempRulesCache[] = "/css_rules.compile.tmp";

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

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

}  // anonymous namespace

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

void CssParser::normalizedInto(const std::string& s, std::string& out) {
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

std::vector<std::string> CssParser::splitOnChar(const std::string& s, const char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;

  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delimiter) {
      std::string part = s.substr(start, i - start);
      std::string trimmed = normalized(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      start = i + 1;
    }
  }
  return parts;
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

CssTextAlign CssParser::interpretAlignment(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string& val) {
  const std::string v = normalized(val);

  // Named values
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  char* endPtr = nullptr;
  const long numericWeight = std::strtol(v.c_str(), &endPtr, 10);

  // If we parsed a number and consumed the whole string
  if (endPtr != v.c_str() && *endPtr == '\0') {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }

  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string& val) {
  const std::string v = normalized(val);

  // text-decoration can have multiple space-separated values
  bool underline = v.find("underline") != std::string::npos;
  bool lineThrough = v.find("line-through") != std::string::npos;
  uint8_t result = 0;
  if (underline) result |= static_cast<uint8_t>(CssTextDecoration::Underline);
  if (lineThrough) result |= static_cast<uint8_t>(CssTextDecoration::LineThrough);
  return static_cast<CssTextDecoration>(result);
}

CssLength CssParser::interpretLength(const std::string& val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(const std::string& val, CssLength& out) {
  const std::string v = normalized(val);
  if (v.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string numPart = v.substr(0, unitStart);
  const std::string unitPart = v.substr(unitStart);

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);
  if (endPtr == numPart.c_str()) {
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

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    style.textDecoration = interpretDecoration(propValueBuf);
    style.defined.textDecoration = 1;
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
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (propNameBuf == "width") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (propNameBuf == "display") {
    const std::string_view displayValue = stripTrailingImportant(propValueBuf);
    style.display = (displayValue == "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  }
}

CssStyle CssParser::parseDeclarations(const std::string& declBlock) {
  CssStyle style;
  std::string propNameBuf;
  std::string propValueBuf;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        const size_t len = i - start;
        std::string decl = declBlock.substr(start, len);
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

void CssParser::processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style) {
  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors
  const auto selectors = splitOnChar(selectorGroup, ',');

  for (const auto& sel : selectors) {
    totalSelectorCandidates_++;
    // Validate selector length before processing
    if (sel.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
      unsupportedSelectorSkips_++;
      continue;
    }

    // Normalize the selector
    std::string key = normalized(sel);
    if (key.empty()) continue;

    // TODO: Consider adding support for sibling css selectors in the future
    // Ensure no + in selector as we don't support adjacent CSS selectors for now
    if (key.find('+') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for direct nested css selectors in the future
    // Ensure no > in selector as we don't support nested CSS selectors for now
    if (key.find('>') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for attribute css selectors in the future
    // Ensure no [ in selector as we don't support attribute CSS selectors for now
    if (key.find('[') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for pseudo selectors in the future
    // Ensure no : in selector as we don't support pseudo CSS selectors for now
    if (key.find(':') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for ID css selectors in the future
    // Ensure no # in selector as we don't support ID CSS selectors for now
    if (key.find('#') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for general sibling combinator selectors in the future
    // Ensure no ~ in selector as we don't support general sibling combinator CSS selectors for now
    if (key.find('~') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Consider adding support for wildcard css selectors in the future
    // Ensure no * in selector as we don't support wildcard CSS selectors for now
    if (key.find('*') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // TODO: Add support for more complex selectors in the future
    // At the moment, we only ever check for `tag`, `tag.class1` or `.class1`
    // If the selector has whitespace in it, then it's either a CSS selector for a descendant element (e.g. `tag1 tag2`)
    // or some other slightly more advanced CSS selector which we don't support yet
    if (key.find(' ') != std::string_view::npos) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // Skip if this would exceed the rule limit
    const size_t ruleCount = compileModeActive_ ? compileSelectorOffsets_.size() : rulesBySelector_.size();
    if (ruleCount >= MAX_RULES) {
      LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
      return;
    }

    if (compileModeActive_) {
      if (!compileTempFile_) {
        compileModeFailed_ = true;
        continue;
      }

      CssStyle merged = style;
      auto existingOffsetIt = compileSelectorOffsets_.find(key);
      if (existingOffsetIt != compileSelectorOffsets_.end()) {
        CssStyle existing;
        FsFile tempRead;
        if (Storage.openFileForRead("CSS", compileTempPath_, tempRead) && tempRead.seek(existingOffsetIt->second) &&
            readCssStylePayload(tempRead, existing)) {
          existing.applyOver(merged);
          merged = existing;
        }
        if (tempRead) {
          tempRead.close();
        }
      }

      const uint32_t styleOffset = compileTempFile_.position();
      writeCssStylePayload(compileTempFile_, merged);
      compileSelectorOffsets_[key] = styleOffset;
      continue;
    }

    // Store or merge with existing (non-compile mode)
    auto it = rulesBySelector_.find(key);
    if (it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[key] = style;
    }
  }
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
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector.str(), currentStyle);
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
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
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

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
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
  compileModeActive_ = true;
  compileModeFailed_ = false;
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

bool CssParser::endCacheCompile() {
  if (!compileModeActive_) {
    return false;
  }

  compileModeActive_ = false;
  compileTempFile_.close();

  if (compileModeFailed_) {
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  FsFile outFile;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, outFile)) {
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  outFile.write(CssParser::CSS_CACHE_VERSION);
  const auto ruleCount = static_cast<uint16_t>(compileSelectorOffsets_.size());
  outFile.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  FsFile tempFile;
  if (!Storage.openFileForRead("CSS", compileTempPath_, tempFile)) {
    outFile.close();
    Storage.remove((cachePath + rulesCache).c_str());
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  std::array<uint8_t, CSS_FIXED_STYLE_BYTES> styleBytes{};
  for (const auto& it : compileSelectorOffsets_) {
    const auto selectorLen = static_cast<uint16_t>(it.first.size());
    outFile.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    outFile.write(reinterpret_cast<const uint8_t*>(it.first.data()), selectorLen);

    if (!tempFile.seek(it.second)) {
      tempFile.close();
      outFile.close();
      Storage.remove((cachePath + rulesCache).c_str());
      Storage.remove(compileTempPath_.c_str());
      compileSelectorOffsets_.clear();
      return false;
    }
    if (tempFile.read(styleBytes.data(), styleBytes.size()) != static_cast<int>(styleBytes.size())) {
      tempFile.close();
      outFile.close();
      Storage.remove((cachePath + rulesCache).c_str());
      Storage.remove(compileTempPath_.c_str());
      compileSelectorOffsets_.clear();
      return false;
    }
    outFile.write(styleBytes.data(), styleBytes.size());
  }

  tempFile.close();
  outFile.close();
  Storage.remove(compileTempPath_.c_str());

  compileSelectorOffsets_.clear();

  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  cacheRuleOffsets_.clear();
  cacheIndexLoaded_ = false;
  cachedRuleCount_ = 0;

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

void CssParser::clear() {
  if (compileTempFile_) {
    compileTempFile_.close();
  }
  rulesBySelector_.clear();
  cacheRuleOffsets_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  cacheIndexLoaded_ = false;
  cachedRuleCount_ = 0;
  resolveStats_ = {};
  compileModeActive_ = false;
  compileModeFailed_ = false;
  compileSelectorOffsets_.clear();
  totalSelectorCandidates_ = 0;
  unsupportedSelectorSkips_ = 0;
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
  return true;
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

void CssParser::cacheHotRule(const std::string& selector, const CssStyle& style) const {
  auto it = hotRuleCache_.find(selector);
  if (it != hotRuleCache_.end()) {
    it->second.first = style;
    touchHotRule(selector);
    return;
  }

  hotRuleLru_.push_front(selector);
  hotRuleCache_.emplace(selector, std::make_pair(style, hotRuleLru_.begin()));
  if (hotRuleCache_.size() > HOT_RULE_CACHE_SIZE) {
    const std::string& evictKey = hotRuleLru_.back();
    hotRuleCache_.erase(evictKey);
    hotRuleLru_.pop_back();
  }
}

bool CssParser::readRuleFromDiskAtOffset(const uint32_t styleOffset, CssStyle& outStyle) const {
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }
  if (!file.seek(styleOffset)) {
    file.close();
    return false;
  }
  const bool ok = readCssStylePayload(file, outStyle);
  file.close();
  return ok;
}

bool CssParser::lookupRule(const std::string& selector, CssStyle& outStyle, const bool allowDiskLookup) const {
  auto mapIt = rulesBySelector_.find(selector);
  if (mapIt != rulesBySelector_.end()) {
    outStyle = mapIt->second;
    resolveStats_.mapHits++;
    return true;
  }

  auto hotIt = hotRuleCache_.find(selector);
  if (hotIt != hotRuleCache_.end()) {
    outStyle = hotIt->second.first;
    touchHotRule(selector);
    resolveStats_.hotHits++;
    return true;
  }

  if (negativeRuleCache_.find(selector) != negativeRuleCache_.end()) {
    resolveStats_.negativeHits++;
    return false;
  }

  if (!allowDiskLookup) {
    resolveStats_.lowHeapDiskBypasses++;
    return false;
  }

  if (!ensureCacheIndexLoaded()) {
    return false;
  }

  const auto offsetIt = cacheRuleOffsets_.find(selector);
  if (offsetIt == cacheRuleOffsets_.end()) {
    if (negativeRuleCache_.size() >= NEGATIVE_CACHE_SIZE) {
      negativeRuleCache_.clear();
    }
    negativeRuleCache_.insert(selector);
    return false;
  }

  if (!readRuleFromDiskAtOffset(offsetIt->second, outStyle)) {
    return false;
  }

  cacheHotRule(selector, outStyle);
  resolveStats_.diskHits++;
  return true;
}

bool CssParser::ensureCacheIndexLoaded() const {
  if (cacheIndexLoaded_) {
    return true;
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

  cacheRuleOffsets_.clear();
  cacheRuleOffsets_.reserve(ruleCount);
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();

  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) || selectorLen == 0 ||
        selectorLen > MAX_SELECTOR_LENGTH) {
      file.close();
      cacheRuleOffsets_.clear();
      return false;
    }

    std::string selector;
    selector.resize(selectorLen);
    if (file.read(&selector[0], selectorLen) != selectorLen) {
      file.close();
      cacheRuleOffsets_.clear();
      return false;
    }

    const uint32_t styleOffset = file.position();
    cacheRuleOffsets_[std::move(selector)] = styleOffset;
    if (!file.seek(styleOffset + CSS_FIXED_STYLE_BYTES)) {
      file.close();
      cacheRuleOffsets_.clear();
      return false;
    }
  }

  cachedRuleCount_ = cacheRuleOffsets_.size();
  cacheIndexLoaded_ = true;
  file.close();
  LOG_DBG("CSS", "Loaded CSS index: %u selectors (hot cache size=%u)", static_cast<unsigned>(cachedRuleCount_),
          static_cast<unsigned>(HOT_RULE_CACHE_SIZE));
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr) const {
  static bool lowHeapWarningLogged = false;
  resolveStats_.resolveCalls++;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const bool lowHeapMode = freeHeap < MIN_FREE_HEAP_FOR_CSS;
  if (lowHeapMode) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      LOG_DBG("CSS", "Warning: low heap (%u bytes) below MIN_FREE_HEAP_FOR_CSS (%u), skipping disk CSS lookups",
              freeHeap, static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    resolveStats_.lowHeapSkips++;
  }
  CssStyle result;
  const std::string tag = normalized(tagName);

  // 1. Apply element-level style (lowest priority)
  {
    CssStyle tagStyle;
    if (lookupRule(tag, tagStyle, !lowHeapMode)) {
      if (lowHeapMode) {
        resolveStats_.lowHeapRescuedHits++;
      }
      result.applyOver(tagStyle);
    }
  }

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority)
  if (!classAttr.empty()) {
    std::string classToken;
    std::string classKey;
    classKey.reserve(32);

    forEachNormalizedClassToken(classAttr, classToken, [&](const std::string& cls) {
      classKey.clear();
      classKey.push_back('.');
      classKey.append(cls);

      CssStyle classStyle;
      if (lookupRule(classKey, classStyle, !lowHeapMode)) {
        if (lowHeapMode) {
          resolveStats_.lowHeapRescuedHits++;
        }
        result.applyOver(classStyle);
      }
    });

    // TODO: Support combinations of classes (e.g. style on p.class1.class2)
    // 3. Apply element.class styles (higher priority)
    std::string combinedKey;
    combinedKey.reserve(tag.size() + 1 + 32);
    forEachNormalizedClassToken(classAttr, classToken, [&](const std::string& cls) {
      combinedKey.clear();
      combinedKey.append(tag);
      combinedKey.push_back('.');
      combinedKey.append(cls);

      CssStyle combinedStyle;
      if (lookupRule(combinedKey, combinedStyle, !lowHeapMode)) {
        if (lowHeapMode) {
          resolveStats_.lowHeapRescuedHits++;
        }
        result.applyOver(combinedStyle);
      }
    });
  }

  if (!result.defined.anySet()) {
    resolveStats_.misses++;
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Write version
  file.write(CssParser::CSS_CACHE_VERSION);

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);

    // Write CssStyle fields (all are POD types)
    const CssStyle& style = pair.second;
    file.write(static_cast<uint8_t>(style.textAlign));
    file.write(static_cast<uint8_t>(style.fontStyle));
    file.write(static_cast<uint8_t>(style.fontWeight));
    file.write(static_cast<uint8_t>(style.textDecoration));

    // Write CssLength fields (value + unit)
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

    // Write defined flags as uint16_t
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
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  file.close();
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  // Drop parse-time in-memory rules, then initialize on-disk selector index.
  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  cacheRuleOffsets_.clear();
  cacheIndexLoaded_ = false;
  cachedRuleCount_ = 0;

  if (!ensureCacheIndexLoaded()) {
    return false;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache index", static_cast<unsigned>(cachedRuleCount_));
  return true;
}
