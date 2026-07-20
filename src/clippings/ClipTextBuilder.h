#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ClipTextBuilder {

struct Word {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int pageIndex = 0;
  uint16_t pageWordIndex = 0;
  std::string text;
  bool paragraphStart = false;
  bool endsWithInsertedHyphen = false;
};

struct Result {
  std::string text;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t wordCount = 0;
};

enum class Status : uint8_t {
  Ok,
  EmptySelection,
  InvalidSelection,
  InvalidUtf8,
  TextTooLong,
};

Status build(const std::vector<Word>& words, const std::vector<uint16_t>& readingOrder, size_t fromOrder,
             size_t toOrder, int startPageInSection, int sectionPageCount, Result& out);

}  // namespace ClipTextBuilder
