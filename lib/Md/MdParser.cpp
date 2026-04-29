#include "MdParser.h"

#include <cctype>

namespace MdParser {

static EpdFontFamily::Style combineFlags(bool bold, bool italic, bool strike) {
  EpdFontFamily::Style result = EpdFontFamily::REGULAR;
  if (bold)
    result =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(result) | static_cast<uint8_t>(EpdFontFamily::BOLD));
  if (italic)
    result =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(result) | static_cast<uint8_t>(EpdFontFamily::ITALIC));
  if (bold && italic)
    result = static_cast<EpdFontFamily::Style>(
        static_cast<uint8_t>(EpdFontFamily::BOLD_ITALIC) |
        (static_cast<uint8_t>(result) & static_cast<uint8_t>(EpdFontFamily::STRIKETHROUGH)));
  if (strike)
    result = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(result) |
                                               static_cast<uint8_t>(EpdFontFamily::STRIKETHROUGH));
  return result;
}

static constexpr int TAB_WIDTH = 4;

static std::string trimLeft(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

static uint8_t parseListIndentLevel(size_t leadingSpaces) {
  // Top-level list markers may be preceded by up to 3 spaces.
  // Nested list items require at least 4 spaces before the marker.
  if (leadingSpaces < TAB_WIDTH) {
    return 0;
  }
  return static_cast<uint8_t>((leadingSpaces - TAB_WIDTH) / TAB_WIDTH + 1);
}

static bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

static bool isUnderscoreEmphasis(const std::string& text, size_t pos, size_t count) {
  if (pos == 0 || pos + count >= text.size()) {
    return true;
  }
  const char before = text[pos - 1];
  const char after = text[pos + count];
  return !(isWordChar(before) && isWordChar(after));
}

static bool isHorizontalRuleLine(const std::string& line) {
  // Must be at least 3 chars of the same marker (-, *, _) with optional spaces
  if (line.size() < 3) return false;

  char marker = 0;
  int count = 0;
  for (char c : line) {
    if (c == ' ' || c == '\t') continue;
    if (c == '-' || c == '*' || c == '_') {
      if (marker == 0) marker = c;
      if (c != marker) return false;
      count++;
    } else {
      return false;
    }
  }
  return count >= 3;
}

std::vector<Span> parseInline(const std::string& text) {
  std::vector<Span> spans;
  std::string current;
  bool bold = false;
  bool italic = false;
  bool strike = false;
  size_t i = 0;

  auto emitSpan = [&]() {
    if (!current.empty()) {
      spans.push_back({std::move(current), combineFlags(bold, italic, strike)});
      current.clear();
    }
  };

  char boldMarker = 0;
  char italicMarker = 0;

  while (i < text.size()) {
    char c = text[i];

    // Escaped character
    if (c == '\\' && i + 1 < text.size()) {
      char next = text[i + 1];
      if (next == '~' && i + 2 < text.size() && text[i + 2] == '~') {
        current.append("~~");
        i += 3;
        continue;
      }
      if (next == '*' || next == '_' || next == '`' || next == '[' || next == '!' || next == '~' || next == '\\') {
        current += next;
        i += 2;
        continue;
      }
    }

    // *** or ___ — toggle both bold and italic when marker matches the current open markers
    if ((c == '*' || c == '_') && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
      if (c == '_' && !isUnderscoreEmphasis(text, i, 3)) {
        current.append(3, c);
        i += 3;
        continue;
      }
      emitSpan();
      if (bold && italic && boldMarker == c && italicMarker == c) {
        bold = false;
        italic = false;
        boldMarker = 0;
        italicMarker = 0;
      } else {
        bold = true;
        italic = true;
        boldMarker = c;
        italicMarker = c;
      }
      i += 3;
      continue;
    }

    // ~~ — toggle strikethrough
    if (c == '~' && i + 1 < text.size() && text[i + 1] == '~') {
      emitSpan();
      strike = !strike;
      i += 2;
      continue;
    }

    // ** or __ — toggle bold
    if ((c == '*' || c == '_') && i + 1 < text.size() && text[i + 1] == c) {
      if (c == '_' && !isUnderscoreEmphasis(text, i, 2)) {
        current.append(2, c);
        i += 2;
        continue;
      }
      emitSpan();
      if (bold && boldMarker == c) {
        bold = false;
        boldMarker = 0;
      } else {
        bold = true;
        boldMarker = c;
      }
      i += 2;
      continue;
    }

    // * or _ — toggle italic
    if (c == '*' || c == '_') {
      if (c == '_' && !isUnderscoreEmphasis(text, i, 1)) {
        current.push_back(c);
        i += 1;
        continue;
      }
      emitSpan();
      if (italic && italicMarker == c) {
        italic = false;
        italicMarker = 0;
      } else {
        italic = true;
        italicMarker = c;
      }
      i += 1;
      continue;
    }

    // Backtick code span — strip backticks, render as regular
    if (c == '`') {
      size_t end = text.find('`', i + 1);
      if (end != std::string::npos) {
        emitSpan();
        spans.push_back({text.substr(i + 1, end - i - 1), EpdFontFamily::REGULAR});
        i = end + 1;
        continue;
      }
      current += c;
      i++;
      continue;
    }

    // Image ![alt](url) — show [alt]
    if (c == '!' && i + 1 < text.size() && text[i + 1] == '[') {
      size_t closeBracket = text.find(']', i + 2);
      if (closeBracket != std::string::npos && closeBracket + 1 < text.size() && text[closeBracket + 1] == '(') {
        size_t closeParen = text.find(')', closeBracket + 2);
        if (closeParen != std::string::npos) {
          std::string alt = text.substr(i + 2, closeBracket - i - 2);
          current += "[";
          current += alt;
          current += "]";
          i = closeParen + 1;
          continue;
        }
      }
      current += c;
      i++;
      continue;
    }

    // Link [text](url) — show text only
    if (c == '[') {
      size_t closeBracket = text.find(']', i + 1);
      if (closeBracket != std::string::npos && closeBracket + 1 < text.size() && text[closeBracket + 1] == '(') {
        size_t closeParen = text.find(')', closeBracket + 2);
        if (closeParen != std::string::npos) {
          current += text.substr(i + 1, closeBracket - i - 1);
          i = closeParen + 1;
          continue;
        }
      }
      current += c;
      i++;
      continue;
    }

    current += c;
    i++;
  }

  emitSpan();

  // If bold/italic were left open, the text had unmatched markers.
  // The spans are still usable — the trailing text just keeps the toggled style.
  return spans;
}

bool isCodeFence(const std::string& line) {
  auto trimmed = trimLeft(line);
  if (trimmed.size() < 3) return false;
  // Must start with ``` (with optional language tag after)
  if (trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') return true;
  // Also support ~~~ fences
  if (trimmed[0] == '~' && trimmed[1] == '~' && trimmed[2] == '~') return true;
  return false;
}

// Detect task list checkbox at start of list content, update prefix accordingly.
// Returns content with the checkbox marker stripped.
static std::string handleTaskList(const std::string& content, std::string& listPrefix) {
  if (content.size() >= 3 && content[0] == '[' && content[2] == ']') {
    char mark = content[1];
    if (mark == 'x' || mark == 'X') {
      listPrefix = "☑ ";
    } else if (mark == ' ') {
      listPrefix = "☐ ";
    } else {
      return content;  // Not a checkbox — keep content as-is
    }
    size_t skip = 3;
    if (skip < content.size() && content[skip] == ' ') skip++;
    return content.substr(skip);
  }
  return content;
}

ParsedLine parseLine(const std::string& rawLine, bool inCodeBlock) {
  ParsedLine result;

  // Inside a code block: either closing fence or verbatim text
  if (inCodeBlock) {
    if (isCodeFence(rawLine)) {
      result.blockType = BlockType::CodeBlock;
      return result;
    }
    result.blockType = BlockType::CodeBlock;
    result.spans.push_back({rawLine, EpdFontFamily::REGULAR});
    return result;
  }

  // Opening code fence
  if (isCodeFence(rawLine)) {
    result.blockType = BlockType::CodeBlock;
    return result;
  }

  // Count leading whitespace for nesting level before trimming.
  // Up to 3 spaces before a list marker are still top-level in CommonMark.
  size_t leadingSpaces = 0;
  for (size_t i = 0; i < rawLine.size(); i++) {
    if (rawLine[i] == ' ')
      leadingSpaces++;
    else if (rawLine[i] == '\t')
      leadingSpaces += TAB_WIDTH;  // Treat tab as 4 spaces to match CommonMark nesting rules
    else
      break;
  }
  result.indentLevel = parseListIndentLevel(leadingSpaces);

  std::string trimmed = trimLeft(rawLine);

  // Blank line
  if (trimmed.empty()) {
    result.blockType = BlockType::BlankLine;
    return result;
  }

  // Horizontal rule (must check BEFORE unordered list since --- and *** overlap)
  if (isHorizontalRuleLine(trimmed)) {
    result.blockType = BlockType::HorizontalRule;
    return result;
  }

  // ATX headers: # H1, ## H2, ### H3+
  if (trimmed[0] == '#') {
    int level = 0;
    size_t pos = 0;
    while (pos < trimmed.size() && trimmed[pos] == '#') {
      level++;
      pos++;
    }
    if (pos < trimmed.size() && trimmed[pos] == ' ') {
      std::string content = trimmed.substr(pos + 1);
      // Strip optional trailing # sequence
      size_t trail = content.size();
      while (trail > 0 && content[trail - 1] == '#') trail--;
      while (trail > 0 && content[trail - 1] == ' ') trail--;
      if (trail < content.size()) content = content.substr(0, trail);

      if (level <= 1)
        result.blockType = BlockType::Header1;
      else if (level == 2)
        result.blockType = BlockType::Header2;
      else
        result.blockType = BlockType::Header3;

      result.spans = parseInline(content);
      // Force bold on all header spans while preserving any existing decoration bits.
      for (auto& span : result.spans) {
        span.style = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(span.style) |
                                                       static_cast<uint8_t>(EpdFontFamily::BOLD));
      }
      return result;
    }
  }

  // Unordered list: - , * , +  (marker followed by space)
  if (trimmed.size() > 1 && trimmed[1] == ' ' && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+')) {
    result.blockType = BlockType::UnorderedList;
    result.listPrefix = "\xe2\x80\xa2 ";  // "• "
    std::string content = handleTaskList(trimmed.substr(2), result.listPrefix);
    result.spans = parseInline(content);
    return result;
  }

  // Ordered list: 1. , 2. , etc. (up to 3-digit number)
  {
    size_t dotPos = trimmed.find(". ");
    if (dotPos != std::string::npos && dotPos <= 3 && dotPos > 0) {
      bool allDigits = true;
      for (size_t j = 0; j < dotPos; j++) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[j]))) {
          allDigits = false;
          break;
        }
      }
      if (allDigits) {
        result.blockType = BlockType::OrderedList;
        result.listPrefix = trimmed.substr(0, dotPos + 2);  // e.g. "1. "
        std::string content = handleTaskList(trimmed.substr(dotPos + 2), result.listPrefix);
        result.spans = parseInline(content);
        return result;
      }
    }
  }

  // Blockquote: > text
  if (trimmed[0] == '>') {
    result.blockType = BlockType::Blockquote;
    std::string content = trimmed.substr(1);
    if (!content.empty() && content[0] == ' ') content = content.substr(1);
    result.spans = parseInline(content);
    return result;
  }

  // Default: paragraph
  result.blockType = BlockType::Paragraph;
  result.spans = parseInline(trimmed);
  return result;
}

}  // namespace MdParser