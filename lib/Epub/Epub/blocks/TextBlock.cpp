#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Serialization.h>

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    Serial.printf("[%lu] [TXB] Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", millis(),
                  (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  auto wordIt = words.begin();
  auto wordStylesIt = wordStyles.begin();
  auto wordXposIt = wordXpos.begin();

  for (size_t i = 0; i < words.size(); i++) {
    renderer.drawText(fontId, *wordXposIt + x, y, wordIt->c_str(), true, *wordStylesIt);

    std::advance(wordIt, 1);
    std::advance(wordStylesIt, 1);
    std::advance(wordXposIt, 1);
  }
}

void TextBlock::serialize(File& file) const {
  // words
  const uint32_t wc = words.size();
  serialization::writePod(file, wc);
  for (const auto& w : words) serialization::writeString(file, w);

  // wordXpos
  const uint32_t xc = wordXpos.size();
  serialization::writePod(file, xc);
  for (auto x : wordXpos) serialization::writePod(file, x);

  // wordStyles
  const uint32_t sc = wordStyles.size();
  serialization::writePod(file, sc);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // style
  serialization::writePod(file, style);
}

std::unique_ptr<TextBlock> TextBlock::deserialize(File& file) {
  uint32_t wc, xc, sc;
  std::list<std::string> words;
  std::list<uint16_t> wordXpos;
  std::list<EpdFontStyle> wordStyles;
  BLOCK_STYLE style;

  // words
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large lists (max 10000 words per block)
  if (wc > 10000) {
    Serial.printf("[%lu] [TXB] Deserialization failed: word count %u exceeds maximum\n", millis(), wc);
    return nullptr;
  }

  words.resize(wc);
  for (auto& w : words) serialization::readString(file, w);

  // wordXpos
  serialization::readPod(file, xc);
  wordXpos.resize(xc);
  for (auto& x : wordXpos) serialization::readPod(file, x);

  // wordStyles
  serialization::readPod(file, sc);
  wordStyles.resize(sc);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Validate data consistency: all three lists must have the same size
  if (wc != xc || wc != sc) {
    Serial.printf("[%lu] [TXB] Deserialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", millis(), wc,
                  xc, sc);
    return nullptr;
  }

  // style
  serialization::readPod(file, style);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), style));
}
