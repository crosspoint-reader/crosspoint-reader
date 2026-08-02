#include <gtest/gtest.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include "src/activities/quiz/core/QuizFormatReader.h"

#ifndef QUIZ_FIXTURES_DIR
#error "QUIZ_FIXTURES_DIR must be defined by the build system"
#endif

namespace {
using namespace quiz;

class FileQuizInput : public QuizInput {
 public:
  explicit FileQuizInput(const std::string& path) : file_(path, std::ios::binary | std::ios::ate) { if (file_) size_ = static_cast<uint64_t>(file_.tellg()); }
  uint64_t size() const override { return size_; }
  bool read(uint64_t offset, void* dst, size_t len) override {
    if (!file_ || offset > size_ || len > size_ - offset) return false;
    file_.seekg(static_cast<std::streamoff>(offset));
    return static_cast<size_t>(file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len)).gcount()) == len;
  }

 private:
  std::ifstream file_; uint64_t size_ = 0;
};

std::string fixturePath(const char* name) { return std::string(QUIZ_FIXTURES_DIR) + "/" + name; }
template <typename T> std::string_view view(const T& bytes, size_t len) { return {bytes.data(), len}; }

TEST(QuizFormatReader, LoadsValidDeckAndQuestionsByOrdinal) {
  FileQuizInput input(fixturePath("valid.quiz")); QuizFormatReader reader(input); QuizDeckInfo deck;
  ASSERT_EQ(reader.validate(&deck), QuizFormatError::Ok);
  EXPECT_EQ(view(deck.title, deck.titleBytes), "Mini Quiz"); EXPECT_EQ(deck.questionCount, 2u);
  EXPECT_NE(deck.deckId[0], 0); EXPECT_NE(deck.revisionId[0], 0);

  QuizQuestionData q;
  ASSERT_EQ(reader.loadQuestion(deck, 1, &q), QuizFormatError::Ok);
  EXPECT_EQ(view(q.prompt, q.promptBytes), "Pi?"); EXPECT_EQ(q.choiceCount, 3); EXPECT_EQ(q.correctChoiceIndex, 1);
  EXPECT_EQ(view(q.choices[0], q.choiceBytes[0]), "2"); EXPECT_EQ(view(q.choices[1], q.choiceBytes[1]), "3.14");
  EXPECT_EQ(view(q.choices[2], q.choiceBytes[2]), "4"); EXPECT_EQ(view(q.explanation, q.explanationBytes), "Approx.");
  ASSERT_EQ(reader.loadQuestion(deck, 0, &q), QuizFormatError::Ok);
  EXPECT_EQ(view(q.prompt, q.promptBytes), "First?"); EXPECT_EQ(q.choiceCount, 2); EXPECT_EQ(q.correctChoiceIndex, 1);
  EXPECT_EQ(view(q.explanation, q.explanationBytes), "");
}

class InvalidFixtureTest : public ::testing::TestWithParam<std::pair<const char*, QuizFormatError>> {};
TEST_P(InvalidFixtureTest, RejectsInvalidDecks) {
  const auto [name, expected] = GetParam();
  FileQuizInput input(fixturePath(name)); QuizFormatReader reader(input); QuizDeckInfo deck;
  EXPECT_EQ(reader.validate(&deck), expected) << name;
}
INSTANTIATE_TEST_SUITE_P(InvalidFixtures, InvalidFixtureTest,
                         ::testing::Values(std::pair{"unsupported_version.quiz", QuizFormatError::UnsupportedVersion},
                                           std::pair{"truncated_header.quiz", QuizFormatError::ReadError},
                                           std::pair{"truncated_index.quiz", QuizFormatError::InvalidHeader},
                                           std::pair{"invalid_offset.quiz", QuizFormatError::InvalidIndex},
                                           std::pair{"invalid_utf8.quiz", QuizFormatError::InvalidText},
                                           std::pair{"embedded_nul.quiz", QuizFormatError::InvalidText},
                                           std::pair{"answer_index_out_of_range.quiz", QuizFormatError::InvalidQuestion},
                                           std::pair{"oversized_prompt.quiz", QuizFormatError::InvalidQuestion},
                                           std::pair{"declared_file_size_mismatch.quiz", QuizFormatError::InvalidHeader},
                                           std::pair{"zero_identity.quiz", QuizFormatError::InvalidHeader},
                                           std::pair{"invalid_index_entry_size.quiz", QuizFormatError::InvalidHeader},
                                           std::pair{"record_header_size_mismatch.quiz", QuizFormatError::InvalidQuestion},
                                           std::pair{"payload_length_mismatch.quiz", QuizFormatError::InvalidQuestion}));
}  // namespace
