#pragma once
#include <string>

#include <EpdFont.h>  // EpdFontFamily::Style

/// Pixel indent applied to bullets, numbered lists, and blockquotes.
constexpr int MD_BULLET_INDENT = 20;

/// A single rendered line produced by the Markdown parser.
struct MdLine {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  int indentPixels = 0;  ///< Left indent for bullets, numbered lists, blockquotes.
  bool isHRule = false;  ///< Draw a horizontal rule instead of text.
};

namespace MdParser {

/// Remove leading/trailing spaces and tabs.
std::string trimWhitespace(const std::string& s);

/// Return true if the trimmed line is a Markdown horizontal rule (3+ of -, *, or _).
bool isHorizontalRule(const std::string& trimmed);

/// Strip inline Markdown markers from a string, keeping visible text only.
/// - Images: ![alt](url)  → [image: alt]
/// - Links:  [text](url)  → text
/// - Bold/italic/code spans stripped, content kept.
std::string stripInlineMarkdown(const std::string& s);

/// Parse a single raw line into an MdLine. Updates inCodeFence state.
/// Pass stripInline=false during page index building to skip the string
/// allocation overhead of stripInlineMarkdown (page breaks stay conservative).
MdLine parseMdLine(const std::string& raw, bool& inCodeFence, bool stripInline = true);

}  // namespace MdParser
