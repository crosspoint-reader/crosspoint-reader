#pragma once

// Markdown line-processing helpers for TxtReaderActivity.
// All ENABLE_MARKDOWN compile-time guards are confined to this feature-owned header so that
// TxtReaderActivity.cpp can call the public functions unconditionally: when markdown support
// is compiled out the functions become transparent no-ops.

#include <FeatureFlags.h>

#include <string>

#include "activities/reader/TxtReaderActivity.h"

namespace features::markdown {

// Returns true when `path` ends with ".md" and markdown support is compiled in.
inline bool isMarkdownPath(const std::string& path) {
#if ENABLE_MARKDOWN
  return path.size() >= 3 && path.compare(path.size() - 3, 3, ".md") == 0;
#else
  (void)path;
  return false;
#endif
}

// Process a raw source line for block-level markdown elements (headings, horizontal rules,
// blockquotes).  Inline markers such as **bold** and *italic* are left intact so that
// word-wrap width measurements remain accurate; strip them at render time with stripInline().
// When `activeMarkdown` is false or ENABLE_MARKDOWN=0, returns a plain StyledLine.
inline TxtReaderActivity::StyledLine processLine(bool activeMarkdown, const std::string& raw) {
  TxtReaderActivity::StyledLine result;
#if ENABLE_MARKDOWN
  if (activeMarkdown) {
    // --- Heading detection: count leading '#' chars followed by a space ---
    int hashCount = 0;
    while (hashCount < static_cast<int>(raw.size()) && raw[hashCount] == '#') {
      hashCount++;
    }
    if (hashCount > 0 && hashCount < static_cast<int>(raw.size()) && raw[hashCount] == ' ') {
      result.style = EpdFontFamily::BOLD;
      result.text = raw.substr(hashCount + 1);
      return result;
    }

    // --- Horizontal rule: a line of >=3 dashes, asterisks, or underscores ---
    auto isHRule = [](const std::string& s, char ch) {
      int count = 0;
      for (char c : s) {
        if (c == ch) {
          count++;
        } else if (c != ' ') {
          return false;
        }
      }
      return count >= 3;
    };
    if (isHRule(raw, '-') || isHRule(raw, '*') || isHRule(raw, '_')) {
      result.isHRule = true;
      return result;
    }

    // --- Blockquote: strip leading "> " or ">" ---
    if (raw.size() >= 2 && raw[0] == '>' && raw[1] == ' ') {
      result.text = raw.substr(2);
    } else if (!raw.empty() && raw[0] == '>') {
      result.text = raw.substr(1);
    } else {
      result.text = raw;
    }
    return result;
  }
#else
  (void)activeMarkdown;
#endif
  result.text = raw;
  return result;
}

// Strip common inline markdown markers from a display string before rendering.
// Should be called at render time so word-wrap measurements (done on raw text) stay accurate.
// When `activeMarkdown` is false or ENABLE_MARKDOWN=0, returns the input unchanged.
inline std::string stripInline(bool activeMarkdown, const std::string& input) {
#if ENABLE_MARKDOWN
  if (!activeMarkdown) {
    return input;
  }
  std::string out;
  out.reserve(input.size());
  const size_t n = input.size();
  size_t i = 0;
  while (i < n) {
    // ** bold ** or __ bold __
    if (i + 1 < n && ((input[i] == '*' && input[i + 1] == '*') || (input[i] == '_' && input[i + 1] == '_'))) {
      const char m = input[i];
      const size_t start = i + 2;
      size_t end = input.find({m, m}, start);
      if (end != std::string::npos) {
        out += input.substr(start, end - start);
        i = end + 2;
        continue;
      }
    }
    // * italic * or _ italic _ (single marker)
    if ((input[i] == '*' || input[i] == '_') && (i == 0 || (input[i - 1] != '*' && input[i - 1] != '_'))) {
      const char m = input[i];
      const size_t start = i + 1;
      size_t end = start;
      while (end < n && input[end] != m) {
        end++;
      }
      if (end < n) {
        out += input.substr(start, end - start);
        i = end + 1;
        continue;
      }
    }
    // `code`
    if (input[i] == '`') {
      const size_t start = i + 1;
      size_t end = input.find('`', start);
      if (end != std::string::npos) {
        out += input.substr(start, end - start);
        i = end + 1;
        continue;
      }
    }
    // [link text](url)
    if (input[i] == '[') {
      const size_t textEnd = input.find(']', i + 1);
      if (textEnd != std::string::npos && textEnd + 1 < n && input[textEnd + 1] == '(') {
        const size_t urlEnd = input.find(')', textEnd + 2);
        if (urlEnd != std::string::npos) {
          out += input.substr(i + 1, textEnd - i - 1);
          i = urlEnd + 1;
          continue;
        }
      }
    }
    out += input[i++];
  }
  return out;
#else
  (void)activeMarkdown;
  return input;
#endif
}

}  // namespace features::markdown
