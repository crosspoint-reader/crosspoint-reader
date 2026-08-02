#pragma once

#include <array>
#include <cstdint>

namespace quiz {

constexpr uint16_t kQuizFormatVersion = 1;
constexpr uint16_t kQuizMaxTitleBytes = 96;
constexpr uint16_t kQuizMaxPromptBytes = 1024;
constexpr uint16_t kQuizMaxChoiceBytes = 384;
constexpr uint16_t kQuizMaxExplanationBytes = 1024;
constexpr uint16_t kQuizHeaderBytes = 176;
constexpr uint16_t kQuizIndexEntryBytes = 8;
constexpr uint16_t kQuizRecordHeaderBytes = 24;
constexpr uint32_t kQuizMaxFileBytes = 64u * 1024u * 1024u;
constexpr uint32_t kQuizMaxQuestions = 10000;
constexpr uint8_t kQuizMinChoices = 2;
constexpr uint8_t kQuizMaxChoices = 6;
constexpr uint8_t kQuizIdentityBytes = 16;
constexpr std::array<uint8_t, 8> kQuizMagicBytes = {0x47, 0x51, 0x55, 0x49, 0x5A, 0x0D, 0x0A, 0x1A};

struct QuizDeckInfo {
  uint32_t declaredFileSize = 0;
  uint32_t questionCount = 0;
  uint32_t indexOffset = 0;
  uint32_t indexBytes = 0;
  uint32_t recordsOffset = 0;
  uint32_t recordsBytes = 0;
  uint16_t titleBytes = 0;
  std::array<uint8_t, kQuizIdentityBytes> deckId{}, revisionId{};
  std::array<char, kQuizMaxTitleBytes + 1> title{};
};

struct QuizQuestionData {
  uint16_t promptBytes = 0, explanationBytes = 0;
  uint8_t choiceCount = 0, correctChoiceIndex = 0;
  std::array<uint16_t, kQuizMaxChoices> choiceBytes{};
  std::array<char, kQuizMaxPromptBytes + 1> prompt{};
  std::array<std::array<char, kQuizMaxChoiceBytes + 1>, kQuizMaxChoices> choices{};
  std::array<char, kQuizMaxExplanationBytes + 1> explanation{};
};

}  // namespace quiz
