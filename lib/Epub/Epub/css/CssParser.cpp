#include "CssParser.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <vector>

namespace {

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to prevent memory exhaustion
constexpr size_t MAX_CSS_RULES = 300;

// Maximum CSS file size to parse (200KB to match Epub.cpp limit)
constexpr size_t MAX_CSS_FILE_SIZE = 200 * 1024;

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Fast ASCII-only tolower (much faster than std::tolower on ESP32)
char fastTolower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

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
      if (braceDepth <= 0) {
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
  // Clear strings but reserve capacity to avoid reallocations
  selector.clear();
  selector.reserve(64);  // Most selectors are short
  body.clear();
  body.reserve(256);  // Most declarations are reasonable

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

  // Find opening brace - scan from current position
  size_t bracePos = pos;
  while (bracePos < css.size() && css[bracePos] != '{') {
    ++bracePos;
  }
  if (bracePos >= css.size()) return false;

  // Extract selector (everything before the brace) - avoid substr allocation
  selector.append(&css[pos], bracePos - pos);

  // Find matching closing brace - scan from brace position
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

  if (depth > 0) return false;  // Unmatched brace

  // Extract body (between braces) - avoid substr allocation
  body.append(&css[bodyStart], bodyEnd - bodyStart - 1);

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
      result.push_back(fastTolower(c));
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

  // Process declarations by semicolon without creating vector
  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      // Skip empty declarations
      if (start >= i) {
        start = i + 1;
        continue;
      }

      // Find colon within this declaration
      size_t colonPos = start;
      while (colonPos < i && declBlock[colonPos] != ':') {
        ++colonPos;
      }

      if (colonPos >= i || colonPos == start) {
        start = i + 1;
        continue;
      }

      // Extract and normalize property name in-place
      std::string propName;
      propName.reserve(32);
      for (size_t j = start; j < colonPos; ++j) {
        const char c = declBlock[j];
        if (!isCssWhitespace(c)) {
          propName.push_back(fastTolower(c));
        }
      }

      if (propName.empty()) {
        start = i + 1;
        continue;
      }

      // Extract and normalize property value in-place
      std::string propValue;
      propValue.reserve(64);
      bool prevWasSpace = false;
      for (size_t j = colonPos + 1; j < i; ++j) {
        const char c = declBlock[j];
        if (!isCssWhitespace(c)) {
          propValue.push_back(fastTolower(c));
          prevWasSpace = false;
        } else if (!prevWasSpace) {
          propValue.push_back(' ');
          prevWasSpace = true;
        }
      }

      // Remove trailing space
      if (!propValue.empty() && propValue.back() == ' ') {
        propValue.pop_back();
      }

      if (propValue.empty()) {
        start = i + 1;
        continue;
      }

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
          style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft =
              1;
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

      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlock(const std::string& selectorGroup, const std::string& declarations) {
  totalRulesProcessed_++;

  // Process comma-separated selectors without creating vector of strings
  // First pass: check which selectors can be added and handle removals
  std::vector<std::string> validSelectors;

  size_t start = 0;
  for (size_t i = 0; i <= selectorGroup.size(); ++i) {
    if (i == selectorGroup.size() || selectorGroup[i] == ',') {
      // Extract and normalize selector in-place
      std::string selector;
      selector.reserve(64);

      bool prevWasSpace = false;
      for (size_t j = start; j < i; ++j) {
        const char c = selectorGroup[j];
        if (!isCssWhitespace(c)) {
          selector.push_back(fastTolower(c));
          prevWasSpace = false;
        } else if (!prevWasSpace) {
          selector.push_back(' ');
          prevWasSpace = true;
        }
      }

      // Remove trailing space
      if (!selector.empty() && selector.back() == ' ') {
        selector.pop_back();
      }

      if (!selector.empty()) {
        // Check if we can add this rule
        bool canAdd = true;
        if (rulesBySelector_.size() >= MAX_CSS_RULES) {
          // At limit - check if this rule has higher priority than our lowest
          int newPriority = calculatePriority(selector);

          if (newPriority > lowestPriorityValue_) {
            // New rule is higher priority - remove lowest and add new
            removeLowestPriorityRule();
            canAdd = true;
          } else {
            // New rule is lower or equal priority - can't add
            canAdd = false;
            rulesIgnoredLowPriority_++;
          }
        }

        if (canAdd) {
          validSelectors.push_back(std::move(selector));
        }
      }
      start = i + 1;
    }
  }

  // Only parse declarations if we have at least one valid selector
  if (validSelectors.empty()) {
    return;
  }

  const CssStyle style = parseDeclarations(declarations);

  // Only store if any properties were set
  if (!style.defined.anySet()) {
    rulesIgnoredNoProperties_++;
    return;
  }

  // Add all valid selectors
  for (const auto& selector : validSelectors) {
    // Store or merge with existing
    auto it = rulesBySelector_.find(selector);
    if (it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[selector] = style;
      // Update lowest priority tracking for new rule
      int priority = calculatePriority(selector);
      if (priority < lowestPriorityValue_) {
        lowestPriorityValue_ = priority;
        lowestPrioritySelector_ = selector;
      }
    }
    rulesAdded_++;
  }
}

// Main parsing entry point

bool CssParser::loadFromString(const std::string& css, size_t fileSize) {
  if (fileSize > MAX_CSS_FILE_SIZE) {
    LOG_ERR("CSS", "CSS file too large (%zu bytes > %zu max), skipping", fileSize, MAX_CSS_FILE_SIZE);
    return false;
  }

  // Reset statistics counters
  totalRulesProcessed_ = 0;
  rulesAdded_ = 0;
  rulesIgnoredLowPriority_ = 0;
  rulesIgnoredNoProperties_ = 0;

  // Reset lowest priority tracking to reflect any rules already in the map
  updateLowestPriorityTracking();

  // Strip comments from the CSS
  std::string cleanedCss = stripComments(css);

  // Parse rules from the cleaned CSS
  size_t pos = 0;
  std::string selector, body;
  while (extractNextRule(cleanedCss, pos, selector, body)) {
    processRuleBlock(selector, body);
  }

  LOG_INF("CSS", "CSS parsing stats - Total: %zu, Added: %zu, Ignored low prio: %zu, Ignored no props: %zu",
          totalRulesProcessed_, rulesAdded_, rulesIgnoredLowPriority_, rulesIgnoredNoProperties_);

  return true;
}

// Main parsing entry point

bool CssParser::parseStreaming(Stream& source) {
#ifdef ENABLE_CSS_HEAP_MONITORING
  const size_t initialFreeHeap = ESP.getFreeHeap();
  LOG_DBG("CSS", "[MEM] Starting CSS parsing - Free heap: %d bytes", initialFreeHeap);
#endif

  // Reset statistics counters
  totalRulesProcessed_ = 0;
  rulesAdded_ = 0;
  rulesIgnoredLowPriority_ = 0;
  rulesIgnoredNoProperties_ = 0;

  // Reset lowest priority tracking to reflect any rules already in the map
  updateLowestPriorityTracking();

  constexpr size_t STREAM_BUFFER_SIZE = 2048;  // Conservative size to prevent memory issues
  std::string buffer;
  buffer.reserve(STREAM_BUFFER_SIZE * 2);  // Allow some overflow

  char chunk[STREAM_BUFFER_SIZE];
  bool inComment = false;
  size_t commentStart = 0;
  size_t totalBytesRead = 0;
  size_t chunkCount = 0;
  size_t extractRuleCalls = 0;
  size_t processRuleCalls = 0;

  // For streams, we read until we get 0 bytes or an error
  while (true) {
    const int bytesRead = source.readBytes(chunk, sizeof(chunk));
    if (bytesRead <= 0) break;

    totalBytesRead += bytesRead;
    chunkCount++;

    // Append to buffer
    buffer.append(chunk, bytesRead);

    // Process comments and rules in the buffer
    size_t pos = 0;
    while (pos < buffer.size()) {
      if (inComment) {
        // Look for end of comment
        const size_t endPos = buffer.find("*/", pos);
        if (endPos != std::string::npos) {
          // Remove the comment
          buffer.erase(commentStart, endPos - commentStart + 2);
          pos = commentStart;
          inComment = false;
        } else {
          // Comment continues, keep the rest
          break;
        }
      } else {
        // Look for start of comment
        const size_t commentPos = buffer.find("/*", pos);
        if (commentPos != std::string::npos) {
          inComment = true;
          commentStart = commentPos;
          pos = commentPos + 2;
        } else {
          // No more comments in this chunk
          break;
        }
      }
    }

    // Try to extract complete rules from the buffer.
    // If we're inside an unfinished comment, only parse the clean prefix
    // (buffer[0..commentStart-1]); going further would treat comment text
    // as CSS, potentially creating bogus rules from comment content.
    const size_t safeParseEnd = inComment ? commentStart : buffer.size();
    size_t parsePos = 0;
    std::string selector, body;
    while (true) {
      const size_t savedPos = parsePos;
      if (!extractNextRule(buffer, parsePos, selector, body)) break;
      if (parsePos > safeParseEnd) {
        // Extraction overstepped into the unfinished comment region; undo
        parsePos = savedPos;
        break;
      }
      extractRuleCalls++;
      processRuleBlock(selector, body);
      processRuleCalls++;
    }

    // Remove processed part of buffer, but keep some overlap for incomplete rules
    if (parsePos > 0) {
      buffer.erase(0, parsePos);
      // Adjust commentStart to account for the bytes just removed.
      // If the opening /* was in the erased prefix, the inComment state is stale
      // and must be cleared; otherwise shift the offset into the new buffer.
      if (inComment) {
        if (commentStart >= parsePos) {
          commentStart -= parsePos;
        } else {
          inComment = false;
        }
      }
    }

    // If buffer is too large, stop processing to avoid memory issues
    if (buffer.size() > STREAM_BUFFER_SIZE * 4) {
      LOG_ERR("CSS", "Buffer overflow in streaming CSS parsing - stopping to prevent memory exhaustion");
      break;  // Don't fail, just stop processing
    }

    // Log memory usage periodically (only when explicitly enabled)
#ifdef ENABLE_CSS_HEAP_MONITORING
    if (totalBytesRead % 131072 == 0) {  // Every 128KB only when monitoring enabled
      const size_t currentFreeHeap = ESP.getFreeHeap();
      LOG_DBG("CSS", "[MEM] Parsed %zu bytes, %zu rules, buffer: %zu bytes, free heap: %d bytes", totalBytesRead,
              rulesBySelector_.size(), buffer.size(), currentFreeHeap);
    }
#endif
  }

  LOG_DBG("CSS", "Streaming parsing: read %zu bytes in %zu chunks, buffer size: %zu, rules processed: %zu",
          totalBytesRead, chunkCount, buffer.size(), totalRulesProcessed_);

  // Process any remaining complete rules
  size_t parsePos = 0;
  std::string selector, body;
  while (extractNextRule(buffer, parsePos, selector, body)) {
    extractRuleCalls++;
    processRuleBlock(selector, body);
    processRuleCalls++;
  }

  LOG_DBG("CSS", "Function calls: extractNextRule=%zu, processRuleBlock=%zu", extractRuleCalls, processRuleCalls);

#ifdef ENABLE_CSS_HEAP_MONITORING
  const size_t finalFreeHeap = ESP.getFreeHeap();
  const size_t heapUsed = initialFreeHeap - finalFreeHeap;
  LOG_DBG("CSS",
          "[MEM] CSS parsing complete - Read %zu bytes, %zu rules, heap used: %d bytes, final free heap: %d bytes",
          totalBytesRead, rulesBySelector_.size(), heapUsed, finalFreeHeap);
#endif
  LOG_INF("CSS", "CSS parsing stats - Total: %zu, Added: %zu, Ignored low prio: %zu, Ignored no props: %zu",
          totalRulesProcessed_, rulesAdded_, rulesIgnoredLowPriority_, rulesIgnoredNoProperties_);

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

  // Update priority tracking to reflect the rules just loaded from cache
  updateLowestPriorityTracking();

  LOG_DBG("CSS", "Loaded %u rules from cache", ruleCount);
  return true;
}

// Calculate priority score for a selector (higher = more important for EPUB rendering)
int CssParser::calculatePriority(const std::string& selector) {
  if (selector.empty()) return 0;

  // Scan once through the selector to check for special characters
  bool hasSpace = false;
  bool hasDot = false;
  bool startsWithDot = false;

  if (!selector.empty() && selector[0] == '.') {
    startsWithDot = true;
  }

  for (char c : selector) {
    if (c == ' ') {
      hasSpace = true;
    } else if (c == '.') {
      hasDot = true;
    } else if (c == '>' || c == '+' || c == '~' || c == ':' || c == '[') {
      // Complex selectors (child/sibling/pseudo/attr) - low priority
      return 1;
    }
  }

  // Descendant selectors (contain a space) are also treated as complex/low priority
  if (hasSpace) {
    return 1;
  }

  // Element.class combination - high priority
  if (hasDot && !startsWithDot) {
    return 9;
  }

  // Class selector - medium high
  if (startsWithDot) {
    return 7;
  }

  // Element selector - medium
  return 5;
}

// Find and remove the lowest priority rule when we need to make space
void CssParser::removeLowestPriorityRule() {
  if (rulesBySelector_.empty()) return;

  // Use tracked lowest priority if available, otherwise recalculate
  if (lowestPrioritySelector_.empty()) {
    updateLowestPriorityTracking();
  }

  if (!lowestPrioritySelector_.empty()) {
    rulesBySelector_.erase(lowestPrioritySelector_);
    // Recompute tracking from the remaining rules so eviction is always accurate
    updateLowestPriorityTracking();
  }
}

// Recalculate lowest priority tracking (expensive, call sparingly)
void CssParser::updateLowestPriorityTracking() {
  lowestPrioritySelector_.clear();
  lowestPriorityValue_ = INT_MAX;

  for (const auto& pair : rulesBySelector_) {
    int priority = calculatePriority(pair.first);
    if (priority < lowestPriorityValue_) {
      lowestPriorityValue_ = priority;
      lowestPrioritySelector_ = pair.first;
    }
  }
}
