#include "MarkdownToHtml.h"

#include <Logging.h>

#include <cstring>

namespace {

constexpr size_t READ_BUF_SIZE = 1024;
constexpr size_t LINE_BUF_SIZE = 512;

void writeStr(FsFile& out, const char* s) { out.write(s, strlen(s)); }

void writeEscapedChar(FsFile& out, char c) {
  switch (c) {
    case '&':
      writeStr(out, "&amp;");
      break;
    case '<':
      writeStr(out, "&lt;");
      break;
    case '>':
      writeStr(out, "&gt;");
      break;
    default:
      out.write(static_cast<uint8_t>(c));
      break;
  }
}

// Process inline markdown formatting and write escaped HTML.
// Handles **bold**, *italic*, `code`, and HTML entity escaping.
void writeInlineFormatted(FsFile& out, const char* line, size_t len) {
  bool inBold = false;
  bool inItalic = false;
  bool inCode = false;

  size_t i = 0;
  while (i < len) {
    // Backtick code spans
    if (line[i] == '`' && !inBold && !inItalic) {
      if (inCode) {
        writeStr(out, "</code>");
        inCode = false;
      } else {
        writeStr(out, "<code>");
        inCode = true;
      }
      i++;
      continue;
    }

    // Inside code: no further formatting, just escape
    if (inCode) {
      writeEscapedChar(out, line[i]);
      i++;
      continue;
    }

    // ** bold
    if (line[i] == '*' && i + 1 < len && line[i + 1] == '*') {
      if (inBold) {
        writeStr(out, "</b>");
        inBold = false;
      } else {
        writeStr(out, "<b>");
        inBold = true;
      }
      i += 2;
      continue;
    }

    // * italic
    if (line[i] == '*') {
      if (inItalic) {
        writeStr(out, "</i>");
        inItalic = false;
      } else {
        writeStr(out, "<i>");
        inItalic = true;
      }
      i++;
      continue;
    }

    writeEscapedChar(out, line[i]);
    i++;
  }

  // Close any unclosed tags
  if (inCode) writeStr(out, "</code>");
  if (inBold) writeStr(out, "</b>");
  if (inItalic) writeStr(out, "</i>");
}

enum class ListType { NONE, UNORDERED, ORDERED };

struct ParserState {
  ListType listType = ListType::NONE;
  bool inBlockquote = false;
  bool inParagraph = false;

  void closeList(FsFile& out) {
    if (listType == ListType::UNORDERED) {
      writeStr(out, "</ul>\n");
    } else if (listType == ListType::ORDERED) {
      writeStr(out, "</ol>\n");
    }
    listType = ListType::NONE;
  }

  void closeBlockquote(FsFile& out) {
    if (inBlockquote) {
      writeStr(out, "</blockquote>\n");
      inBlockquote = false;
    }
  }

  void closeParagraph(FsFile& out) {
    if (inParagraph) {
      writeStr(out, "</p>\n");
      inParagraph = false;
    }
  }

  void closeAll(FsFile& out) {
    closeParagraph(out);
    closeList(out);
    closeBlockquote(out);
  }
};

// Returns number of leading '#' characters (0-6), or 0 if not a heading
int getHeadingLevel(const char* line, size_t len) {
  int level = 0;
  while (level < 6 && static_cast<size_t>(level) < len && line[level] == '#') {
    level++;
  }
  // Must be followed by space or end of line
  if (level > 0 && (static_cast<size_t>(level) >= len || line[level] == ' ')) {
    return level;
  }
  return 0;
}

bool isHorizontalRule(const char* line, size_t len) {
  if (len < 3) return false;
  // Must be 3+ of same char (-, *, _) with optional spaces
  int count = 0;
  char ruleChar = 0;
  for (size_t i = 0; i < len; i++) {
    if (line[i] == ' ') continue;
    if (line[i] == '-' || line[i] == '*' || line[i] == '_') {
      if (ruleChar == 0) ruleChar = line[i];
      if (line[i] != ruleChar) return false;
      count++;
    } else {
      return false;
    }
  }
  return count >= 3;
}

bool isUnorderedListItem(const char* line, size_t len, size_t& contentStart) {
  if (len >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
    contentStart = 2;
    return true;
  }
  return false;
}

bool isOrderedListItem(const char* line, size_t len, size_t& contentStart) {
  size_t i = 0;
  while (i < len && line[i] >= '0' && line[i] <= '9') {
    i++;
  }
  if (i > 0 && i < len && line[i] == '.' && i + 1 < len && line[i + 1] == ' ') {
    contentStart = i + 2;
    return true;
  }
  return false;
}

bool isBlockquote(const char* line, size_t len, size_t& contentStart) {
  if (len >= 1 && line[0] == '>') {
    contentStart = 1;
    if (len >= 2 && line[1] == ' ') contentStart = 2;
    return true;
  }
  return false;
}

void processLine(FsFile& out, const char* line, size_t len, ParserState& state) {
  // Trim trailing whitespace
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\n')) {
    len--;
  }

  // Empty line: close current blocks, start new paragraph on next content
  if (len == 0) {
    state.closeAll(out);
    return;
  }

  // Horizontal rule
  if (isHorizontalRule(line, len)) {
    state.closeAll(out);
    writeStr(out, "<hr/>\n");
    return;
  }

  // Heading
  int headingLevel = getHeadingLevel(line, len);
  if (headingLevel > 0) {
    state.closeAll(out);
    char tag[8];
    snprintf(tag, sizeof(tag), "<h%d>", headingLevel);
    writeStr(out, tag);
    size_t contentStart = headingLevel;
    if (contentStart < len && line[contentStart] == ' ') contentStart++;
    writeInlineFormatted(out, line + contentStart, len - contentStart);
    snprintf(tag, sizeof(tag), "</h%d>\n", headingLevel);
    writeStr(out, tag);
    return;
  }

  // Blockquote
  size_t contentStart = 0;
  if (isBlockquote(line, len, contentStart)) {
    state.closeParagraph(out);
    state.closeList(out);
    if (!state.inBlockquote) {
      writeStr(out, "<blockquote>");
      state.inBlockquote = true;
    }
    writeStr(out, "<p>");
    writeInlineFormatted(out, line + contentStart, len - contentStart);
    writeStr(out, "</p>\n");
    return;
  }

  // If we were in a blockquote but this line isn't one, close it
  if (state.inBlockquote) {
    state.closeBlockquote(out);
  }

  // Unordered list
  if (isUnorderedListItem(line, len, contentStart)) {
    state.closeParagraph(out);
    if (state.listType == ListType::ORDERED) state.closeList(out);
    if (state.listType != ListType::UNORDERED) {
      writeStr(out, "<ul>\n");
      state.listType = ListType::UNORDERED;
    }
    writeStr(out, "<li>");
    writeInlineFormatted(out, line + contentStart, len - contentStart);
    writeStr(out, "</li>\n");
    return;
  }

  // Ordered list
  if (isOrderedListItem(line, len, contentStart)) {
    state.closeParagraph(out);
    if (state.listType == ListType::UNORDERED) state.closeList(out);
    if (state.listType != ListType::ORDERED) {
      writeStr(out, "<ol>\n");
      state.listType = ListType::ORDERED;
    }
    writeStr(out, "<li>");
    writeInlineFormatted(out, line + contentStart, len - contentStart);
    writeStr(out, "</li>\n");
    return;
  }

  // Close list if we're no longer in one
  if (state.listType != ListType::NONE) {
    state.closeList(out);
  }

  // Regular paragraph text
  if (!state.inParagraph) {
    writeStr(out, "<p>");
    state.inParagraph = true;
  } else {
    // Continuation line within same paragraph — add a space
    writeStr(out, " ");
  }
  writeInlineFormatted(out, line, len);
}

}  // namespace

bool MarkdownToHtml::convert(const std::string& mdPath, const std::string& htmlPath) {
  FsFile inFile, outFile;

  if (!Storage.openFileForRead("MD", mdPath, inFile)) {
    LOG_ERR("MD", "Failed to open markdown file: %s", mdPath.c_str());
    return false;
  }

  if (!Storage.openFileForWrite("MD", htmlPath, outFile)) {
    LOG_ERR("MD", "Failed to open output HTML file: %s", htmlPath.c_str());
    inFile.close();
    return false;
  }

  writeStr(outFile, "<html><body>\n");

  uint8_t* readBuf = static_cast<uint8_t*>(malloc(READ_BUF_SIZE));
  char* lineBuf = static_cast<char*>(malloc(LINE_BUF_SIZE));
  if (!readBuf || !lineBuf) {
    LOG_ERR("MD", "Failed to allocate buffers");
    free(readBuf);
    free(lineBuf);
    inFile.close();
    outFile.close();
    return false;
  }

  ParserState state;
  size_t lineLen = 0;

  while (inFile.available()) {
    size_t bytesRead = inFile.read(readBuf, READ_BUF_SIZE);
    for (size_t i = 0; i < bytesRead; i++) {
      char c = static_cast<char>(readBuf[i]);
      if (c == '\n') {
        lineBuf[lineLen] = '\0';
        processLine(outFile, lineBuf, lineLen, state);
        lineLen = 0;
      } else if (lineLen < LINE_BUF_SIZE - 1) {
        lineBuf[lineLen++] = c;
      }
      // else: line too long, silently truncate
    }
  }

  // Process final line if no trailing newline
  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    processLine(outFile, lineBuf, lineLen, state);
  }

  state.closeAll(outFile);
  writeStr(outFile, "</body></html>\n");

  free(readBuf);
  free(lineBuf);
  inFile.close();
  outFile.close();

  LOG_DBG("MD", "Converted %s -> %s", mdPath.c_str(), htmlPath.c_str());
  return true;
}
