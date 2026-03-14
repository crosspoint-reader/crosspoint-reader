#include <EpdFontFamily.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "GfxRenderer.h"
#include "../../lib/Epub/Epub/ParsedText.h"
#include "../../lib/Epub/Epub/blocks/BlockStyle.h"

namespace {

[[noreturn]] void fail(const char* message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

void expect(const bool condition, const char* message) {
  if (!condition) {
    fail(message);
  }
}

std::shared_ptr<TextBlock> singleLineFrom(ParsedText& parsedText, const int pageWidth) {
  GfxRenderer renderer;
  std::shared_ptr<TextBlock> line;
  parsedText.layoutAndExtractLines(renderer, 0, static_cast<uint16_t>(pageWidth),
                                   [&line](const std::shared_ptr<TextBlock>& block) { line = block; });
  expect(static_cast<bool>(line), "expected a line to be extracted");
  return line;
}

void testLtrPositionsRemainLeftOrigin() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  ParsedText parsedText(true, false, style, false);
  parsedText.addWord("aaa", EpdFontFamily::REGULAR);
  parsedText.addWord("bb", EpdFontFamily::REGULAR);
  parsedText.addWord("c", EpdFontFamily::REGULAR);

  const auto line = singleLineFrom(parsedText, 100);
  const auto& xpos = line->getWordXpos();
  expect(!line->getIsRtl(), "expected LTR line metadata");
  expect(xpos.size() == 3, "expected three LTR word positions");
  expect(xpos[0] == 0 && xpos[1] == 35 && xpos[2] == 60, "unexpected LTR word positions");
}

void testRtlPositionsStartFromRightEdge() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  ParsedText parsedText(true, false, style, true);
  parsedText.addWord("aaa", EpdFontFamily::REGULAR);
  parsedText.addWord("bb", EpdFontFamily::REGULAR);
  parsedText.addWord("c", EpdFontFamily::REGULAR);

  const auto line = singleLineFrom(parsedText, 100);
  const auto& xpos = line->getWordXpos();
  expect(line->getIsRtl(), "expected RTL line metadata");
  expect(xpos.size() == 3, "expected three RTL word positions");
  expect(xpos[0] == 70 && xpos[1] == 45 && xpos[2] == 30, "unexpected RTL word positions");
}

void testRtlTextIndentUsesRightLeadingEdge() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndent = 10;
  style.textIndentDefined = true;
  ParsedText parsedText(false, false, style, true);
  parsedText.addWord("aaa", EpdFontFamily::REGULAR);
  parsedText.addWord("bb", EpdFontFamily::REGULAR);

  const auto line = singleLineFrom(parsedText, 100);
  const auto& xpos = line->getWordXpos();
  expect(xpos.size() == 2, "expected two RTL word positions with indent");
  expect(xpos[0] == 60 && xpos[1] == 35, "unexpected RTL indented word positions");
}

}  // namespace

int main() {
  testLtrPositionsRemainLeftOrigin();
  testRtlPositionsStartFromRightEdge();
  testRtlTextIndentUsesRightLeadingEdge();
  return 0;
}