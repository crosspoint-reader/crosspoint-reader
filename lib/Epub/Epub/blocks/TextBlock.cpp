#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      (!wordBackgrounds.empty() && words.size() != wordBackgrounds.size())) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, bg=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(), (uint32_t)wordBackgrounds.size());
    return;
  }

  const int ascender = renderer.getFontAscenderSize(fontId);
  const int lineHeight = renderer.getLineHeight(fontId);
  const int pad = 2;

  // Pass 1: Draw merged background fills for consecutive same-color runs
  size_t i = 0;
  while (i < words.size()) {
    uint8_t runColor = wordBackgrounds.empty() ? 0 : wordBackgrounds[i];
    if (runColor == 0) {
      ++i;
      continue;
    }

    // Start a run at word i
    const size_t runStart = i;
    int runLeft = wordXpos[runStart] + x;
    int runRight = runLeft + renderer.getTextAdvanceX(fontId, words[runStart].c_str(), wordStyles[runStart]);

    ++i;
    while (i < words.size()) {
      uint8_t nextColor = wordBackgrounds.empty() ? 0 : wordBackgrounds[i];
      if (nextColor != runColor) break;

      // Extend run to include word i (and everything between previous word and this word)
      const int nextLeft = wordXpos[i] + x;
      const int nextRight = nextLeft + renderer.getTextAdvanceX(fontId, words[i].c_str(), wordStyles[i]);
      if (nextLeft < runLeft) runLeft = nextLeft;
      if (nextRight > runRight) runRight = nextRight;
      ++i;
    }

    const int bgX = runLeft - pad;
    const int bgY = y - pad;
    const int bgW = (runRight - runLeft) + pad * 2;
    const int bgH = lineHeight + pad * 2;
    renderer.fillRectDither(bgX, bgY, bgW, bgH, static_cast<Color>(runColor));
  }

  // Pass 2: Draw text (foreground)
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    // Choose text color based on word background: if background is dark (>8), draw white (false)
    const uint8_t bgColor = wordBackgrounds.empty() ? 0 : wordBackgrounds[i];
    const bool drawBlackText = (bgColor == 0) || (bgColor <= 8);
    renderer.drawText(fontId, wordX, y, words[i].c_str(), drawBlackText, currentStyle);

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }
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

  // Per-word background colors (length-prefixed: 0 words = legacy, 0xFFFF = sentinel if needed)
  serialization::writePod(file, static_cast<uint16_t>(wordBackgrounds.size()));
  for (auto bg : wordBackgrounds) serialization::writePod(file, bg);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserializeLegacy(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  // Read words with bounded allocation to prevent corrupt disk data from causing unbounded memory allocation
  for (auto& w : words) {
    uint32_t strLen;
    serialization::readPod(file, strLen);
    if (strLen > 1024) {  // Maximum reasonable word length
      LOG_ERR("TXB", "Deserialization failed: word length %u exceeds maximum", strLen);
      return nullptr;
    }
    if (strLen > 0) {
      // Read into temporary buffer first to avoid partial fills on short reads
      auto tempBuf = static_cast<char*>(malloc(strLen));
      if (!tempBuf) {
        LOG_ERR("TXB", "Deserialization failed: malloc failed for %u bytes", strLen);
        return nullptr;
      }
      const size_t bytesRead = file.read(tempBuf, strLen);
      if (bytesRead != strLen) {
        free(tempBuf);
        LOG_ERR("TXB", "Deserialization failed: short read (got %u, expected %u)", (uint32_t)bytesRead, strLen);
        return nullptr;
      }
      w.assign(tempBuf, strLen);
      free(tempBuf);
    }
  }
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

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

  auto tb = std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle));
  LOG_DBG("TXB", "deserializeLegacy: wc=%u bg=all-zero", wc);
  return tb;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file, uint8_t sectionVersion) {
  if (sectionVersion < 22) {
    return deserializeLegacy(file);
  }

  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  serialization::readPod(file, wc);

  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  // Read words with bounded allocation to prevent corrupt disk data from causing unbounded memory allocation
  for (auto& w : words) {
    uint32_t strLen;
    serialization::readPod(file, strLen);
    if (strLen > 1024) {  // Maximum reasonable word length
      LOG_ERR("TXB", "Deserialization failed: word length %u exceeds maximum", strLen);
      return nullptr;
    }
    if (strLen > 0) {
      // Read into temporary buffer first to avoid partial fills on short reads
      auto tempBuf = static_cast<char*>(malloc(strLen));
      if (!tempBuf) {
        LOG_ERR("TXB", "Deserialization failed: malloc failed for %u bytes", strLen);
        return nullptr;
      }
      const size_t bytesRead = file.read(tempBuf, strLen);
      if (bytesRead != strLen) {
        free(tempBuf);
        LOG_ERR("TXB", "Deserialization failed: short read (got %u, expected %u)", (uint32_t)bytesRead, strLen);
        return nullptr;
      }
      w.assign(tempBuf, strLen);
      free(tempBuf);
    }
  }
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

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

  uint16_t bgCount;
  serialization::readPod(file, bgCount);
  LOG_DBG("TXB", "deserialize v22: wc=%u bgCount=%u", wc, bgCount);
  std::vector<uint8_t> wordBackgrounds;
  if (bgCount == wc) {
    wordBackgrounds.resize(bgCount);
    for (auto& bg : wordBackgrounds) serialization::readPod(file, bg);
  } else {
    wordBackgrounds.resize(wc, 0);  // mismatch: default to all transparent
    if (bgCount > 0) {
      for (uint16_t i = 0; i < bgCount; i++) {
        uint8_t dummy;
        serialization::readPod(file, dummy);
      }
    }
  }

  auto tb = std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle));
  tb->wordBackgrounds = std::move(wordBackgrounds);
  return tb;
}
