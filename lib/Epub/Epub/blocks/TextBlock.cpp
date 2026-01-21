#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Serialization.h>
#include <Utf8.h>

// Debug logging for Thai rendering investigation
// Set to 1 to enable verbose TextBlock render logging
#define TEXTBLOCK_DEBUG_LOGGING 0

// Validates a UTF-8 string for proper encoding
// Returns true if valid, false if corruption detected
static bool validateUtf8String(const std::string& str) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(str.data());
  const uint8_t* end = ptr + str.size();

  while (ptr < end) {
    if (*ptr == 0) {
      // Null byte in middle of string indicates corruption
      return false;
    }

    int bytes = utf8CodepointLen(*ptr);

    // Check we have enough bytes remaining
    if (ptr + bytes > end) {
      return false;
    }

    // Validate continuation bytes
    for (int i = 1; i < bytes; i++) {
      if ((ptr[i] & 0xC0) != 0x80) {
        return false;
      }
    }

    ptr += bytes;
  }

  return true;
}

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

int TextBlock::validateAllWords(const char* checkpoint) const {
  int corruptCount = 0;
  size_t idx = 0;
  for (const auto& word : words) {
    if (!validateUtf8String(word)) {
      corruptCount++;
      Serial.printf("[%lu] [TXB] CORRUPT @ %s word[%u] len=%u bytes: ", millis(), checkpoint, (uint32_t)idx,
                    (uint32_t)word.size());
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(word.data());
      for (size_t j = 0; j < word.size() && j < 16; j++) {
        Serial.printf("%02X ", bytes[j]);
      }
      Serial.printf("\n");
    }
    idx++;
  }
  return corruptCount;
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
  size_t wordIdx = 0;
  for (auto& w : words) {
    serialization::readString(file, w);
#if TEXTBLOCK_DEBUG_LOGGING
    // Check for corruption immediately after deserialization
    if (!validateUtf8String(w)) {
      Serial.printf("[%lu] [TXB] !! CORRUPT ON DESERIALIZE word[%u] len=%u, bytes: ",
                    millis(), (uint32_t)wordIdx, (uint32_t)w.size());
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(w.data());
      for (size_t j = 0; j < w.size() && j < 16; j++) {
        Serial.printf("%02X ", bytes[j]);
      }
      Serial.printf("\n");
    }
#endif
    wordIdx++;
  }
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Block style
  serialization::readPod(file, style);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), style));
}
