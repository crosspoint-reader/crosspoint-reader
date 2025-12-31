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

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    Serial.printf("[%lu] [TXB] Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", millis(),
                  words.size(), wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Block style
  serialization::writePod(file, style);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::list<std::string> words;
  std::list<uint16_t> wordXpos;
  std::list<EpdFontFamily::Style> wordStyles;
  Style style;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large lists (max 10000 words per block)
  if (wc > 10000) {
    Serial.printf("[%lu] [TXB] Deserialization failed: word count %u exceeds maximum\n", millis(), wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Block style
  serialization::readPod(file, style);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), style));
}
