#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>

void TextBlock::collectCodepoints(std::vector<uint32_t>& out, size_t max) const {
  if (max == 0 || out.size() >= max) {
    return;
  }

  for (const auto& word : words) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&ptr))) {
      // Check if already exists (simple linear search, OK for small sets)
      bool exists = false;
      for (uint32_t existing : out) {
        if (existing == cp) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        out.push_back(cp);
        if (out.size() >= max) {
          return;
        }
      }
    }
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const int viewportWidth) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  const int effectiveFontId = (blockStyle.fontId != 0) ? blockStyle.fontId : fontId;


  for (size_t i = 0; i < words.size(); i++) {
    const EpdFontFamily::Style currentStyle = wordStyles[i];

    if (isVertical && i < wordYpos.size()) {
      // 縦書きモード: xはカラム位置、yはwordYposから
      renderer.drawTextVertical(effectiveFontId, x + wordXpos[i], y + wordYpos[i],
                                words[i].c_str(), true, currentStyle);
    } else {
      const int wordX = wordXpos[i] + x;
      renderer.drawText(effectiveFontId, wordX, y, words[i].c_str(), true, currentStyle);

      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const std::string& w = words[i];
        const int fullWordWidth = renderer.getTextWidth(effectiveFontId, w.c_str(), currentStyle);
        // y is the top of the text line; add ascender to reach baseline, then offset 2px below
        const int underlineY = y + renderer.getFontAscenderSize(effectiveFontId) + 2;

        int startX = wordX;
        int underlineWidth = fullWordWidth;

        // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
        if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
            static_cast<uint8_t>(w[2]) == 0x83) {
          const char* visiblePtr = w.c_str() + 3;
          const int prefixWidth = renderer.getTextAdvanceX(effectiveFontId, "\xe2\x80\x83", currentStyle);
          const int visibleWidth = renderer.getTextWidth(effectiveFontId, visiblePtr, currentStyle);
          startX = wordX + prefixWidth;
          underlineWidth = visibleWidth;
        }

        renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
      }
    }
  }

  // Draw full-width separator line below the block (used for h1/h2 headings)
  if (blockStyle.drawSeparatorBelow && viewportWidth > 0) {
    const int separatorY = y + renderer.getLineHeight(effectiveFontId) + 2;
    renderer.drawLine(0, separatorY, viewportWidth, separatorY, true);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", words.size(),
            wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.fontId);
  serialization::writePod(file, blockStyle.drawSeparatorBelow);
  serialization::writePod(file, blockStyle.isListItem);

  // Vertical layout data
  serialization::writePod(file, isVertical);
  if (isVertical) {
    for (auto y : wordYpos) serialization::writePod(file, y);
  }

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.fontId);
  serialization::readPod(file, blockStyle.drawSeparatorBelow);
  serialization::readPod(file, blockStyle.isListItem);

  // Vertical layout data
  bool vertical = false;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  if (vertical) {
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
  }

  return std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle,
                    std::move(wordYpos), vertical));
}
