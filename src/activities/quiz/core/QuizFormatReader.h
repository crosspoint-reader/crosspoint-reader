#pragma once
#include "QuizFormat.h"
#include <cstddef>
#include <cstdint>

namespace quiz {
class QuizInput {
 public:
  virtual ~QuizInput() = default;
  virtual uint64_t size() const = 0;
  virtual bool read(uint64_t offset, void* dst, size_t len) = 0;
};
enum class QuizFormatError : uint8_t { Ok, ReadError, UnsupportedVersion, InvalidHeader, InvalidIndex, InvalidQuestion, InvalidText };
class QuizFormatReader {
 public:
  explicit QuizFormatReader(QuizInput& input) : input_(input) {}
  QuizFormatError validate(QuizDeckInfo* outDeck);
  QuizFormatError loadQuestion(const QuizDeckInfo& deck, uint32_t ordinal, QuizQuestionData* outQuestion);

 private:
  QuizInput& input_;
};
}  // namespace quiz
