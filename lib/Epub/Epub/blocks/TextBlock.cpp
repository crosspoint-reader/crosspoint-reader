#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Serialization.h>

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  auto wordIt = words.begin();
  auto wordStylesIt = wordStyles.begin();
  auto wordXposIt = wordXpos.begin();

  for (int i = 0; i < words.size(); i++) {
    renderer.drawText(fontId, *wordXposIt + x, y, wordIt->c_str(), true, *wordStylesIt);

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
  std::list<EpdFontStyle> wordStyles;
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
