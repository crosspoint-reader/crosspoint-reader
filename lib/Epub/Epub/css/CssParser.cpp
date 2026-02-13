#include "CssParser.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace {

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum CSS file size we'll process (prevent memory issues)
constexpr size_t MAX_CSS_SIZE = 64 * 1024;

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Read entire file into string (with size limit)
std::string readFileContent(FsFile& file) {
  std::string content;
  content.reserve(std::min(static_cast<size_t>(file.size()), MAX_CSS_SIZE));

  char buffer[READ_BUFFER_SIZE];
  while (file.available() && content.size() < MAX_CSS_SIZE) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;
    content.append(buffer, bytesRead);
  }
  return content;
}

// Remove CSS comments (/* ... */) from content
std::string stripComments(const std::string& css) {
  std::string result;
  result.reserve(css.size());

  size_t pos = 0;
  while (pos < css.size()) {
    // Look for start of comment
    if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
      // Find end of comment
      const size_t endPos = css.find("*/", pos + 2);
      if (endPos == std::string::npos) {
        // Unterminated comment - skip rest of file
        break;
      }
      pos = endPos + 2;
    } else {
      result.push_back(css[pos]);
      ++pos;
    }
  }
  return result;
}

// Skip @-rules (like @media, @import, @font-face)
// Returns position after the @-rule
size_t skipAtRule(const std::string& css, const size_t start) {
  // Find the end - either semicolon (simple @-rule) or matching brace
  size_t pos = start + 1;  // Skip the '@'

  // Skip identifier
  while (pos < css.size() && (std::isalnum(css[pos]) || css[pos] == '-')) {
    ++pos;
  }

  // Look for { or ;
  int braceDepth = 0;
  while (pos < css.size()) {
    const char c = css[pos];
    if (c == '{') {
      ++braceDepth;
    } else if (c == '}') {
      --braceDepth;
      if (braceDepth == 0) {
        return pos + 1;
      }
    } else if (c == ';' && braceDepth == 0) {
      return pos + 1;
    }
    ++pos;
  }
  return css.size();
}

// Extract next rule from CSS content
// Returns true if a rule was found, with selector and body filled
bool extractNextRule(const std::string& css, size_t& pos, std::string& selector, std::string& body) {
  selector.clear();
  body.clear();

  // Skip whitespace and @-rules until we find a regular rule
  while (pos < css.size()) {
    // Skip whitespace
    while (pos < css.size() && isCssWhitespace(css[pos])) {
      ++pos;
    }

    if (pos >= css.size()) return false;

    // Handle @-rules iteratively (avoids recursion/stack overflow)
    if (css[pos] == '@') {
      pos = skipAtRule(css, pos);
      continue;  // Try again after skipping the @-rule
    }

    break;  // Found start of a regular rule
  }

  if (pos >= css.size()) return false;

  // Find opening brace
  const size_t bracePos = css.find('{', pos);
  if (bracePos == std::string::npos) return false;

  // Extract selector (everything before the brace)
  selector = css.substr(pos, bracePos - pos);

  // Find matching closing brace
  int depth = 1;
  const size_t bodyStart = bracePos + 1;
  size_t bodyEnd = bodyStart;

  while (bodyEnd < css.size() && depth > 0) {
    if (css[bodyEnd] == '{')
      ++depth;
    else if (css[bodyEnd] == '}')
      --depth;
    ++bodyEnd;
  }

  // Extract body (between braces)
  if (bodyEnd > bodyStart) {
    body = css.substr(bodyStart, bodyEnd - bodyStart - 1);
  }

  pos = bodyEnd;
  return true;
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
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
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
  if (v.find("underline") != std::string::npos) {
    return CssTextDecoration::Underline;
  }
  return CssTextDecoration::None;
}

CssLength CssParser::interpretLength(const std::string& val) {
  const std::string v = normalized(val);
  if (v.empty()) return CssLength{};

  // Find where the number ends
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

  // Parse numeric value
  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);
  if (endPtr == numPart.c_str()) return CssLength{};  // No number parsed

  // Determine unit type (preserve for deferred resolution)
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
  // px and unitless default to Pixels

  return CssLength{numericValue, unit};
}

int8_t CssParser::interpretSpacing(const std::string& val) {
  const std::string v = normalized(val);
  if (v.empty()) return 0;

  // For spacing, we convert to "lines" (discrete units for e-ink)
  // 1em ≈ 1 line, percentages based on ~30 lines per page

  float multiplier = 0.0f;
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

  if (unitPart == "em" || unitPart == "rem") {
    multiplier = 1.0f;  // 1em = 1 line
  } else if (unitPart == "%") {
    multiplier = 0.3f;  // ~30 lines per page, so 10% = 3 lines
  } else {
    return 0;  // Unsupported unit for spacing
  }

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);

  if (endPtr == numPart.c_str()) return 0;

  int lines = static_cast<int>(numericValue * multiplier);

  // Clamp to reasonable range (0-2 lines)
  if (lines < 0) lines = 0;
  if (lines > 2) lines = 2;

  return static_cast<int8_t>(lines);
}

// Declaration parsing

CssStyle CssParser::parseDeclarations(const std::string& declBlock) {
  CssStyle style;

  // Split declarations by semicolon
  const auto declarations = splitOnChar(declBlock, ';');

  for (const auto& decl : declarations) {
    // Find colon separator
    const size_t colonPos = decl.find(':');
    if (colonPos == std::string::npos || colonPos == 0) continue;

    std::string propName = normalized(decl.substr(0, colonPos));
    std::string propValue = normalized(decl.substr(colonPos + 1));

    if (propName.empty() || propValue.empty()) continue;

    // Match property and set value
    if (propName == "text-align") {
      style.textAlign = interpretAlignment(propValue);
      style.defined.textAlign = 1;
    } else if (propName == "font-style") {
      style.fontStyle = interpretFontStyle(propValue);
      style.defined.fontStyle = 1;
    } else if (propName == "font-weight") {
      style.fontWeight = interpretFontWeight(propValue);
      style.defined.fontWeight = 1;
    } else if (propName == "text-decoration" || propName == "text-decoration-line") {
      style.textDecoration = interpretDecoration(propValue);
      style.defined.textDecoration = 1;
    } else if (propName == "text-indent") {
      style.textIndent = interpretLength(propValue);
      style.defined.textIndent = 1;
    } else if (propName == "margin-top") {
      style.marginTop = interpretLength(propValue);
      style.defined.marginTop = 1;
    } else if (propName == "margin-bottom") {
      style.marginBottom = interpretLength(propValue);
      style.defined.marginBottom = 1;
    } else if (propName == "margin-left") {
      style.marginLeft = interpretLength(propValue);
      style.defined.marginLeft = 1;
    } else if (propName == "margin-right") {
      style.marginRight = interpretLength(propValue);
      style.defined.marginRight = 1;
    } else if (propName == "margin") {
      // Shorthand: 1-4 values for top, right, bottom, left
      const auto values = splitWhitespace(propValue);
      if (!values.empty()) {
        style.marginTop = interpretLength(values[0]);
        style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
        style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
        style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
        style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
      }
    } else if (propName == "padding-top") {
      style.paddingTop = interpretLength(propValue);
      style.defined.paddingTop = 1;
    } else if (propName == "padding-bottom") {
      style.paddingBottom = interpretLength(propValue);
      style.defined.paddingBottom = 1;
    } else if (propName == "padding-left") {
      style.paddingLeft = interpretLength(propValue);
      style.defined.paddingLeft = 1;
    } else if (propName == "padding-right") {
      style.paddingRight = interpretLength(propValue);
      style.defined.paddingRight = 1;
    } else if (propName == "padding") {
      // Shorthand: 1-4 values for top, right, bottom, left
      const auto values = splitWhitespace(propValue);
      if (!values.empty()) {
        style.paddingTop = interpretLength(values[0]);
        style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
        style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
        style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
        style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom =
            style.defined.paddingLeft = 1;
      }
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlock(const std::string& selectorGroup, const std::string& declarations) {
  const CssStyle style = parseDeclarations(declarations);

  // Only store if any properties were set
  if (!style.defined.anySet()) return;

  // Handle comma-separated selectors
  const auto selectors = splitOnChar(selectorGroup, ',');

  for (const auto& sel : selectors) {
    // Normalize the selector
    std::string key = normalized(sel);
    if (key.empty()) continue;

    // Store or merge with existing
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

  // Read file content
  const std::string content = readFileContent(source);
  if (content.empty()) {
    return true;  // Empty file is valid
  }

  // Remove comments
  const std::string cleaned = stripComments(content);

  // Parse rules
  size_t pos = 0;
  std::string selector, body;

  while (extractNextRule(cleaned, pos, selector, body)) {
    processRuleBlock(selector, body);
  }

  LOG_DBG("CSS", "Parsed %zu rules", rulesBySelector_.size());
  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr) const {
  CssStyle result;
  const std::string tag = normalized(tagName);

  // 1. Apply element-level style (lowest priority)
  const auto tagIt = rulesBySelector_.find(tag);
  if (tagIt != rulesBySelector_.end()) {
    result.applyOver(tagIt->second);
  }

  // 2. Apply class styles (medium priority)
  if (!classAttr.empty()) {
    const auto classes = splitWhitespace(classAttr);

    for (const auto& cls : classes) {
      std::string classKey = "." + normalized(cls);

      auto classIt = rulesBySelector_.find(classKey);
      if (classIt != rulesBySelector_.end()) {
        result.applyOver(classIt->second);
      }
    }

    // 3. Apply element.class styles (higher priority)
    for (const auto& cls : classes) {
      std::string combinedKey = tag + "." + normalized(cls);

      auto combinedIt = rulesBySelector_.find(combinedKey);
      if (combinedIt != rulesBySelector_.end()) {
        result.applyOver(combinedIt->second);
      }
    }
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache format version - increment when format changes
constexpr uint8_t CSS_CACHE_VERSION = 2;

bool CssParser::saveToCache(FsFile& file) const {
  if (!file) {
    return false;
  }

  // Write version
  file.write(CSS_CACHE_VERSION);

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
    file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  return true;
}

bool CssParser::loadFromCache(FsFile& file) {
  if (!file) {
    return false;
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u)", version, CSS_CACHE_VERSION);
    return false;
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    // Read selector string
    uint16_t selectorLen = 0;
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      rulesBySelector_.clear();
      return false;
    }

    std::string selector;
    selector.resize(selectorLen);
    if (file.read(&selector[0], selectorLen) != selectorLen) {
      rulesBySelector_.clear();
      return false;
    }

    // Read CssStyle fields
    CssStyle style;
    uint8_t enumVal;

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textAlign = static_cast<CssTextAlign>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontStyle = static_cast<CssFontStyle>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontWeight = static_cast<CssFontWeight>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textDecoration = static_cast<CssTextDecoration>(enumVal);

    // Read CssLength fields
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
        !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read defined flags
    uint16_t definedBits = 0;
    if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
      rulesBySelector_.clear();
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

    rulesBySelector_[selector] = style;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache", ruleCount);
  return true;
}
