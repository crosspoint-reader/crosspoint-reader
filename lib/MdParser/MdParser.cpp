#include "MdParser.h"

#include <algorithm>
#include <string>

namespace MdParser {

std::string trimWhitespace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
  return s.substr(start, end - start);
}

bool isHorizontalRule(const std::string& trimmed) {
  if (trimmed.size() < 3) return false;
  const char c = trimmed[0];
  if (c != '-' && c != '*' && c != '_') return false;
  return std::all_of(trimmed.begin(), trimmed.end(), [c](char ch) { return ch == c; });
}

// Strip inline Markdown markers, showing link text only (not the URL).
std::string stripInlineMarkdown(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    // Image: ![alt](url) → [image: alt]
    if (i + 1 < s.size() && s[i] == '!' && s[i + 1] == '[') {
      size_t altEnd = s.find(']', i + 2);
      if (altEnd != std::string::npos && altEnd + 1 < s.size() && s[altEnd + 1] == '(') {
        size_t urlEnd = s.find(')', altEnd + 2);
        if (urlEnd != std::string::npos) {
          result += "[image: ";
          result += s.substr(i + 2, altEnd - (i + 2));
          result += ']';
          i = urlEnd + 1;
          continue;
        }
      }
      result += s[i++];
      continue;
    }
    // Link: [text](url) → text (link text only, no URL)
    if (s[i] == '[') {
      size_t textEnd = s.find(']', i + 1);
      if (textEnd != std::string::npos && textEnd + 1 < s.size() && s[textEnd + 1] == '(') {
        size_t urlEnd = s.find(')', textEnd + 2);
        if (urlEnd != std::string::npos) {
          result += s.substr(i + 1, textEnd - (i + 1));
          i = urlEnd + 1;
          continue;
        }
      }
      result += s[i++];
      continue;
    }
    // Bold+italic: ***text***
    if (i + 2 < s.size() && s[i] == '*' && s[i + 1] == '*' && s[i + 2] == '*') {
      size_t end = s.find("***", i + 3);
      if (end != std::string::npos) {
        result += s.substr(i + 3, end - (i + 3));
        i = end + 3;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Bold: **text**
    if (i + 1 < s.size() && s[i] == '*' && s[i + 1] == '*') {
      size_t end = s.find("**", i + 2);
      if (end != std::string::npos) {
        result += s.substr(i + 2, end - (i + 2));
        i = end + 2;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Italic: *text*
    if (s[i] == '*') {
      size_t end = s.find('*', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Bold alt: __text__
    if (i + 1 < s.size() && s[i] == '_' && s[i + 1] == '_') {
      size_t end = s.find("__", i + 2);
      if (end != std::string::npos) {
        result += s.substr(i + 2, end - (i + 2));
        i = end + 2;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Italic alt: _text_ — only when preceded by space or at line start
    if (s[i] == '_' && (i == 0 || s[i - 1] == ' ')) {
      size_t end = s.find('_', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    // Inline code: `text`
    if (s[i] == '`') {
      size_t end = s.find('`', i + 1);
      if (end != std::string::npos) {
        result += s.substr(i + 1, end - (i + 1));
        i = end + 1;
        continue;
      }
      result += s[i++];
      continue;
    }
    result += s[i++];
  }
  return result;
}

// Parse a single raw line into an MdLine. Updates inCodeFence state.
// Pass stripInline=false during page index building to skip the string
// allocation overhead of stripInlineMarkdown (width slightly overestimated,
// but page breaks are conservative and content is not affected).
MdLine parseMdLine(const std::string& raw, bool& inCodeFence, bool stripInline) {
  MdLine result;
  const std::string trimmed = trimWhitespace(raw);

  // Code fence toggle: line starts with ```
  if (trimmed.size() >= 3 && trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') {
    inCodeFence = !inCodeFence;
    return result;  // Empty line for the fence delimiter itself
  }

  // Inside code fence — verbatim, no Markdown parsing
  if (inCodeFence) {
    result.text = raw;
    return result;
  }

  // Horizontal rule: trimmed line is 3+ of same char (-, *, _)
  if (isHorizontalRule(trimmed)) {
    result.isHRule = true;
    return result;
  }

  // Heading: 1–6 leading '#' chars followed by a space
  if (!trimmed.empty() && trimmed[0] == '#') {
    size_t level = 0;
    while (level < trimmed.size() && trimmed[level] == '#') level++;
    if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
      result.style = EpdFontFamily::BOLD;
      const std::string body = trimWhitespace(trimmed.substr(level + 1));
      result.text = stripInline ? stripInlineMarkdown(body) : body;
      return result;
    }
  }

  // Bullet: "- ", "+ " (not "* " to avoid ambiguity with HRule before this check)
  if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '+') && trimmed[1] == ' ') {
    result.indentPixels = MD_BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = "\xe2\x80\xa2 " + (stripInline ? stripInlineMarkdown(body) : body);  // UTF-8 •
    return result;
  }
  // Bullet: "* " (safe here since HRule "***" was already matched above)
  if (trimmed.size() >= 2 && trimmed[0] == '*' && trimmed[1] == ' ') {
    result.indentPixels = MD_BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = "\xe2\x80\xa2 " + (stripInline ? stripInlineMarkdown(body) : body);
    return result;
  }

  // Numbered list: digits followed by ". "
  if (!trimmed.empty() && trimmed[0] >= '1' && trimmed[0] <= '9') {
    size_t k = 0;
    while (k < trimmed.size() && trimmed[k] >= '0' && trimmed[k] <= '9') k++;
    if (k < trimmed.size() && trimmed[k] == '.' && k + 1 < trimmed.size() && trimmed[k + 1] == ' ') {
      result.indentPixels = MD_BULLET_INDENT;
      const std::string body = trimWhitespace(trimmed.substr(k + 2));
      result.text = trimmed.substr(0, k + 1) + " " + (stripInline ? stripInlineMarkdown(body) : body);
      return result;
    }
  }

  // Blockquote: "> "
  if (trimmed.size() >= 2 && trimmed[0] == '>' && trimmed[1] == ' ') {
    result.style = EpdFontFamily::ITALIC;
    result.indentPixels = MD_BULLET_INDENT;
    const std::string body = trimWhitespace(trimmed.substr(2));
    result.text = stripInline ? stripInlineMarkdown(body) : body;
    return result;
  }

  // Normal paragraph text
  result.text = stripInline ? stripInlineMarkdown(raw) : raw;
  return result;
}

}  // namespace MdParser
