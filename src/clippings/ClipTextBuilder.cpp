#include "ClipTextBuilder.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <string_view>

#include "ClippingCodec.h"

namespace ClipTextBuilder {
namespace {

struct CleanWord {
  std::string text;
  bool startsParagraph = false;
  bool joinsNext = false;
};

bool utf8SpaceAt(const std::string_view text, const size_t index, size_t& bytes, bool& emSpace) {
  emSpace = false;
  const uint8_t first = static_cast<uint8_t>(text[index]);
  if (first <= 0x7F && std::isspace(first)) {
    bytes = 1;
    return true;
  }
  if (first == 0xC2 && index + 1 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0xA0) {
    bytes = 2;
    return true;
  }
  if (first == 0xE2 && index + 2 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0x80) {
    const uint8_t last = static_cast<uint8_t>(text[index + 2]);
    if (last >= 0x80 && last <= 0x8A) {
      bytes = 3;
      emSpace = last == 0x83;
      return true;
    }
    if (last == 0xAF) {
      bytes = 3;
      return true;
    }
  }
  return false;
}

Status cleanWord(const Word& word, CleanWord& out) {
  out = {};
  if (!ClippingCodec::isValidUtf8(word.text)) return Status::InvalidUtf8;

  bool sawVisible = false;
  bool pendingSpace = false;
  out.text.reserve(word.text.size());
  for (size_t i = 0; i < word.text.size();) {
    size_t bytes = 0;
    bool emSpace = false;
    if (utf8SpaceAt(word.text, i, bytes, emSpace)) {
      if (!sawVisible && emSpace) out.startsParagraph = true;
      pendingSpace = sawVisible;
      i += bytes;
      continue;
    }
    if (pendingSpace && !out.text.empty()) out.text.push_back(' ');
    pendingSpace = false;
    sawVisible = true;

    const uint8_t first = static_cast<uint8_t>(word.text[i]);
    size_t codePointBytes = 1;
    if ((first & 0xE0U) == 0xC0U) codePointBytes = 2;
    if ((first & 0xF0U) == 0xE0U) codePointBytes = 3;
    if ((first & 0xF8U) == 0xF0U) codePointBytes = 4;
    out.text.append(word.text, i, codePointBytes);
    i += codePointBytes;
  }

  if (word.endsWithInsertedHyphen && !out.text.empty() && out.text.back() == '-') {
    out.text.pop_back();
    out.joinsNext = true;
  }
  return Status::Ok;
}

bool startsWithAny(const std::string_view text, const std::initializer_list<std::string_view> values) {
  return std::any_of(values.begin(), values.end(), [&](const std::string_view value) {
    return text.size() >= value.size() && text.substr(0, value.size()) == value;
  });
}

bool endsWithAny(const std::string_view text, const std::initializer_list<std::string_view> values) {
  return std::any_of(values.begin(), values.end(), [&](const std::string_view value) {
    return text.size() >= value.size() && text.substr(text.size() - value.size()) == value;
  });
}

bool isClosingPunctuation(const std::string_view text) {
  return startsWithAny(text,
                       {",", ".", ";", ":", "!", "?", "%", ")", "]", "}", "'", "\"", "’", "”", "»", "…", "、", "。"});
}

bool isOpeningPunctuation(const std::string_view text) {
  return endsWithAny(text, {"(", "[", "{", "'", "\"", "‘", "“", "«"});
}

bool isWordBoundaryByte(const uint8_t byte) { return byte >= 0x80 || std::isalnum(byte); }

bool visuallyAttachedFragment(const Word& previous, const Word& current, const std::string_view previousText,
                              const std::string_view currentText) {
  if (previous.pageIndex != current.pageIndex || previous.y != current.y || previousText.empty() ||
      currentText.empty()) {
    return false;
  }
  const int previousEnd = previous.x + std::max(0, previous.width);
  if (current.x > previousEnd) return false;
  const bool bothWordLike = isWordBoundaryByte(static_cast<uint8_t>(previousText.back())) &&
                            isWordBoundaryByte(static_cast<uint8_t>(currentText.front()));
  return !bothWordLike;
}

bool addPart(std::string& text, const std::string_view separator, const std::string_view word) {
  if (separator.size() + word.size() > ClippingCodec::MAX_TEXT_BYTES - text.size()) return false;
  text.append(separator);
  text.append(word);
  return true;
}

}  // namespace

Status build(const std::vector<Word>& words, const std::vector<uint16_t>& readingOrder, const size_t fromOrder,
             const size_t toOrder, const int startPageInSection, const int sectionPageCount, Result& out) {
  out = {};
  if (readingOrder.empty() || fromOrder > toOrder || toOrder >= readingOrder.size() || startPageInSection < 0 ||
      sectionPageCount <= 0 || sectionPageCount > std::numeric_limits<uint16_t>::max()) {
    return Status::InvalidSelection;
  }

  bool haveWord = false;
  const Word* previousWord = nullptr;
  CleanWord previousClean;
  uint16_t selectedWordCount = 0;
  for (size_t orderIndex = fromOrder; orderIndex <= toOrder; ++orderIndex) {
    const uint16_t wordIndex = readingOrder[orderIndex];
    if (wordIndex >= words.size()) return Status::InvalidSelection;
    const Word& word = words[wordIndex];
    if (word.pageIndex < 0 || word.pageIndex >= sectionPageCount ||
        startPageInSection > std::numeric_limits<uint16_t>::max() - word.pageIndex) {
      return Status::InvalidSelection;
    }

    CleanWord clean;
    const Status cleanStatus = cleanWord(word, clean);
    if (cleanStatus != Status::Ok) return cleanStatus;
    if (clean.text.empty()) continue;

    const uint16_t absolutePage = static_cast<uint16_t>(startPageInSection + word.pageIndex);
    if (absolutePage >= sectionPageCount) return Status::InvalidSelection;
    if (!haveWord) {
      out.startWordIndex = wordIndex;
      out.startPage = absolutePage;
      out.startPageWordIndex = word.pageWordIndex;
      if (!addPart(out.text, {}, clean.text)) return Status::TextTooLong;
      haveWord = true;
    } else {
      const bool authoredHyphenJoin = !previousClean.text.empty() && previousClean.text.back() == '-' &&
                                      isWordBoundaryByte(static_cast<uint8_t>(clean.text.front()));
      std::string_view separator = " ";
      if (clean.startsParagraph || word.paragraphStart) {
        separator = "\n";
      } else if (previousClean.joinsNext || authoredHyphenJoin) {
        separator = {};
      } else if (isClosingPunctuation(clean.text) || isOpeningPunctuation(previousClean.text) ||
                 visuallyAttachedFragment(*previousWord, word, previousClean.text, clean.text)) {
        separator = {};
      }
      if (!addPart(out.text, separator, clean.text)) return Status::TextTooLong;
    }

    out.endWordIndex = wordIndex;
    if (absolutePage == out.startPage) {
      out.startPageWordIndex = std::min(out.startPageWordIndex, word.pageWordIndex);
    }
    if (absolutePage < out.endPage) return Status::InvalidSelection;
    if (absolutePage == out.endPage) {
      out.endPageWordIndex = std::max(out.endPageWordIndex, word.pageWordIndex);
    } else {
      out.endPage = absolutePage;
      out.endPageWordIndex = word.pageWordIndex;
    }
    if (selectedWordCount == std::numeric_limits<uint16_t>::max()) return Status::InvalidSelection;
    ++selectedWordCount;
    previousWord = &word;
    previousClean = std::move(clean);
  }

  if (!haveWord) return Status::EmptySelection;
  out.pageCount = static_cast<uint16_t>(sectionPageCount);
  out.wordCount = selectedWordCount;
  return Status::Ok;
}

}  // namespace ClipTextBuilder
