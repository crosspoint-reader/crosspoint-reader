#include <EpdFontFamily.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include "../../lib/Epub/Epub/ParsedText.h"
#include "../../lib/Epub/Epub/blocks/BlockStyle.h"
#include "GfxRenderer.h"

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
  const auto& words = line->getWords();
  expect(line->getIsRtl(), "expected RTL line metadata");
  expect(xpos.size() == 1, "expected contiguous Latin text in RTL to merge into one rendered segment");
  expect(words.size() == 1 && words[0] == "aaa bb c", "expected merged Latin text to preserve spaces");
  expect(xpos[0] == 30, "unexpected RTL merged-run position");
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
  const auto& words = line->getWords();
  expect(xpos.size() == 1, "expected indented RTL Latin text to merge into one rendered segment");
  expect(words.size() == 1 && words[0] == "aaa bb", "expected merged indented run to preserve spaces");
  expect(xpos[0] == 35, "unexpected RTL indented merged-run position");
}

void testRtlLineKeepsEmbeddedLtrWordsLeftToRight() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  ParsedText parsedText(true, false, style, true);
  parsedText.addWord(u8"نص", EpdFontFamily::REGULAR);
  parsedText.addWord("(Pomerantz", EpdFontFamily::REGULAR);
  parsedText.addWord("2015)", EpdFontFamily::REGULAR);
  parsedText.addWord(u8"آخر", EpdFontFamily::REGULAR);

  const auto line = singleLineFrom(parsedText, 320);
  const auto& xpos = line->getWordXpos();
  const auto& words = line->getWords();
  expect(xpos.size() == 3, "expected embedded LTR run to merge into a single rendered segment");
  expect(words.size() == 3, "expected three rendered words after merging the embedded LTR run");
  expect(words[1] == "(Pomerantz 2015)", "expected embedded citation text to preserve spaces and parentheses");
}

void testRtlLineMergesMultiWordLtrCitationRuns() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  ParsedText parsedText(true, false, style, true);
  parsedText.addWord(u8"مثال", EpdFontFamily::REGULAR);
  parsedText.addWord("(Mutchler", EpdFontFamily::REGULAR);
  parsedText.addWord("and", EpdFontFamily::REGULAR);
  parsedText.addWord("2014)", EpdFontFamily::REGULAR);
  parsedText.addWord(u8"آخر", EpdFontFamily::REGULAR);

  const auto line = singleLineFrom(parsedText, 360);
  const auto& words = line->getWords();
  expect(words.size() == 3, "expected multi-word citation run to merge into one rendered segment");
  expect(words[1] == "(Mutchler and 2014)", "expected merged citation run to keep its original LTR ordering");
}

}  // namespace

int main() {
  testLtrPositionsRemainLeftOrigin();
  testRtlPositionsStartFromRightEdge();
  testRtlTextIndentUsesRightLeadingEdge();
  testRtlLineKeepsEmbeddedLtrWordsLeftToRight();
  testRtlLineMergesMultiWordLtrCitationRuns();
  return 0;
}
