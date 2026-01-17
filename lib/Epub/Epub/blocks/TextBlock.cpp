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
  auto wordUnderlineIt = wordUnderlines.begin();
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = *wordXposIt + x;
    renderer.drawText(fontId, wordX, y, wordIt->c_str(), true, *wordStylesIt);

    // Draw underline if word is underlined
    if (wordUnderlineIt != wordUnderlines.end() && *wordUnderlineIt) {
      const std::string& w = *wordIt;
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), *wordStylesIt);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getIndentWidth(fontId, std::string("\xe2\x80\x83").c_str());
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, *wordStylesIt);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }

    std::advance(wordIt, 1);
    std::advance(wordStylesIt, 1);
    std::advance(wordXposIt, 1);
    if (wordUnderlineIt != wordUnderlines.end()) {
      std::advance(wordUnderlineIt, 1);
    }
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

  // Underline flags (packed as bytes, 8 words per byte)
  uint8_t underlineByte = 0;
  int bitIndex = 0;
  auto underlineIt = wordUnderlines.begin();
  for (size_t i = 0; i < words.size(); i++) {
    if (underlineIt != wordUnderlines.end() && *underlineIt) {
      underlineByte |= 1 << bitIndex;
    }
    bitIndex++;
    if (bitIndex == 8 || i == words.size() - 1) {
      serialization::writePod(file, underlineByte);
      underlineByte = 0;
      bitIndex = 0;
    }
    if (underlineIt != wordUnderlines.end()) {
      ++underlineIt;
    }
  }

  // Block style (alignment)
  serialization::writePod(file, style);

  // Block style (margins/padding/indent)
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.textIndent);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::list<std::string> words;
  std::list<uint16_t> wordXpos;
  std::list<EpdFontFamily::Style> wordStyles;
  std::list<bool> wordUnderlines;
  Style style;
  BlockStyle blockStyle;

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

  // Underline flags (packed as bytes, 8 words per byte)
  wordUnderlines.resize(wc, false);
  auto underlineIt = wordUnderlines.begin();
  const int bytesNeeded = (wc + 7) / 8;
  for (int byteIdx = 0; byteIdx < bytesNeeded; byteIdx++) {
    uint8_t underlineByte;
    serialization::readPod(file, underlineByte);
    for (int bit = 0; bit < 8 && underlineIt != wordUnderlines.end(); bit++) {
      *underlineIt = (underlineByte & 1 << bit) != 0;
      ++underlineIt;
    }
  }

  // Block style (alignment)
  serialization::readPod(file, style);

  // Block style (margins/padding/indent)
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.textIndent);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), style,
                                                  blockStyle, std::move(wordUnderlines)));
}
