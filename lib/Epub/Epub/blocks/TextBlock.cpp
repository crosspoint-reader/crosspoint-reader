#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Serialization.h>

void TextBlock::addWord(std::string word, const bool is_bold, const bool is_italic) {
  if (word.length() == 0) return;

  words.push_back(std::move(word));
  wordStyles.push_back((is_bold ? BOLD_SPAN : 0) | (is_italic ? ITALIC_SPAN : 0));
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  auto wordIt = words.begin();
  auto wordStylesIt = wordStyles.begin();
  auto wordXposIt = wordXpos.begin();

  for (int i = 0; i < words.size(); i++) {
    // render the word
    EpdFontStyle fontStyle = REGULAR;
    if (*wordStylesIt & BOLD_SPAN && *wordStylesIt & ITALIC_SPAN) {
      fontStyle = BOLD_ITALIC;
    } else if (*wordStylesIt & BOLD_SPAN) {
      fontStyle = BOLD;
    } else if (*wordStylesIt & ITALIC_SPAN) {
      fontStyle = ITALIC;
    }
    renderer.drawText(fontId, *wordXposIt + x, y, wordIt->c_str(), true, fontStyle);

    std::advance(wordIt, 1);
    std::advance(wordStylesIt, 1);
    std::advance(wordXposIt, 1);
  }
}

void TextBlock::serialize(std::ostream& os) const {
  // words
  const uint32_t wc = words.size();
  serialization::writePod(os, wc);
  for (const auto& w : words) serialization::writeString(os, w);

  // wordXpos
  const uint32_t xc = wordXpos.size();
  serialization::writePod(os, xc);
  for (auto x : wordXpos) serialization::writePod(os, x);

  // wordStyles
  const uint32_t sc = wordStyles.size();
  serialization::writePod(os, sc);
  for (auto s : wordStyles) serialization::writePod(os, s);

  // style
  serialization::writePod(os, style);
}

std::unique_ptr<TextBlock> TextBlock::deserialize(std::istream& is) {
  uint32_t wc, xc, sc;
  std::list<std::string> words;
  std::list<uint16_t> wordXpos;
  std::list<uint8_t> wordStyles;
  BLOCK_STYLE style;

  // words
  serialization::readPod(is, wc);
  words.resize(wc);
  for (auto& w : words) serialization::readString(is, w);

  // wordXpos
  serialization::readPod(is, xc);
  wordXpos.resize(xc);
  for (auto& x : wordXpos) serialization::readPod(is, x);

  // wordStyles
  serialization::readPod(is, sc);
  wordStyles.resize(sc);
  for (auto& s : wordStyles) serialization::readPod(is, s);

  // style
  serialization::readPod(is, style);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), style));
}
