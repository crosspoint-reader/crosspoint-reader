#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

namespace MdParser {

struct Span {
  std::string text;
  EpdFontFamily::Style style;
};

enum class BlockType : uint8_t {
  Paragraph,
  Header1,
  Header2,
  Header3,
  UnorderedList,
  OrderedList,
  Blockquote,
  CodeBlock,
  HorizontalRule,
  BlankLine,
  TableHeader,     // | col | col | — header row
  TableSeparator,  // | --- | --- | — ignored visually
  TableRow,        // | val | val | — data row
};

struct ParsedLine {
  BlockType blockType = BlockType::Paragraph;
  std::vector<Span> spans;
  std::string listPrefix;   // "• " or "1. " etc.
  uint8_t indentLevel = 0;  // Nesting depth (each 4 spaces = 1 level)
  // For TableHeader / TableRow: one entry per cell (already inline-parsed)
  std::vector<std::vector<Span>> tableCells;
};

// Returns true if the line is a GFM pipe-table row (| ... |) or separator (| --- |).
bool isTableRow(const std::string& line);
// Returns true if the line is a GFM table separator row (| :---: | --- |).
bool isTableSeparator(const std::string& line);

// Parse a single raw line of markdown into block type and styled spans.
// |inCodeBlock| indicates whether the line is inside a fenced code block.
ParsedLine parseLine(const std::string& rawLine, bool inCodeBlock);

// Returns true if the line is a code fence (``` with optional language tag).
bool isCodeFence(const std::string& line);

// Parse inline markdown formatting (bold, italic, code spans, links, images).
std::vector<Span> parseInline(const std::string& text);

}  // namespace MdParser