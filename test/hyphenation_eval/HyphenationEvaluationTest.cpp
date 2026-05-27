#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined by the build system"
#endif

namespace {

struct TestCase {
  std::string word;
  std::string hyphenated;
  std::vector<size_t> expectedPositions;
  int frequency;
};

struct EvaluationResult {
  int truePositives = 0;
  int falsePositives = 0;
  int falseNegatives = 0;
  double precision = 0.0;
  double recall = 0.0;
  double f1Score = 0.0;
  double weightedScore = 0.0;
};

std::vector<size_t> expectedPositionsFromAnnotatedWord(const std::string& annotated) {
  std::vector<size_t> positions;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(annotated.c_str());
  size_t codepointIndex = 0;

  while (*ptr != 0) {
    if (*ptr == '=') {
      positions.push_back(codepointIndex);
      ++ptr;
      continue;
    }

    utf8NextCodepoint(&ptr);
    ++codepointIndex;
  }

  return positions;
}

std::vector<TestCase> loadTestData(const std::string& filename) {
  std::vector<TestCase> testCases;
  std::ifstream file(filename);

  if (!file.is_open()) {
    return testCases;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream iss(line);
    std::string word, hyphenated, freqStr;

    if (std::getline(iss, word, '|') && std::getline(iss, hyphenated, '|') && std::getline(iss, freqStr, '|')) {
      TestCase testCase;
      testCase.word = word;
      testCase.hyphenated = hyphenated;
      testCase.frequency = std::stoi(freqStr);
      testCase.expectedPositions = expectedPositionsFromAnnotatedWord(hyphenated);
      testCases.push_back(testCase);
    }
  }

  return testCases;
}

std::string positionsToHyphenated(const std::string& word, const std::vector<size_t>& positions) {
  std::string result;
  std::vector<size_t> sortedPositions = positions;
  std::sort(sortedPositions.begin(), sortedPositions.end());

  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  size_t codepointIndex = 0;
  size_t posIdx = 0;

  while (*ptr != 0) {
    while (posIdx < sortedPositions.size() && sortedPositions[posIdx] == codepointIndex) {
      result.push_back('=');
      ++posIdx;
    }

    const unsigned char* current = ptr;
    utf8NextCodepoint(&ptr);
    result.append(reinterpret_cast<const char*>(current), reinterpret_cast<const char*>(ptr));
    ++codepointIndex;
  }

  while (posIdx < sortedPositions.size() && sortedPositions[posIdx] == codepointIndex) {
    result.push_back('=');
    ++posIdx;
  }

  return result;
}

std::vector<size_t> hyphenateWordWithHyphenator(const std::string& word, const LanguageHyphenator& hyphenator) {
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);
  return hyphenator.breakIndexes(cps);
}

EvaluationResult evaluateWord(const TestCase& testCase, const std::vector<size_t>& actualPositions) {
  EvaluationResult result;

  std::vector<size_t> expected = testCase.expectedPositions;
  std::vector<size_t> actual = actualPositions;

  std::sort(expected.begin(), expected.end());
  std::sort(actual.begin(), actual.end());

  for (size_t pos : actual) {
    if (std::find(expected.begin(), expected.end(), pos) != expected.end()) {
      result.truePositives++;
    } else {
      result.falsePositives++;
    }
  }

  for (size_t pos : expected) {
    if (std::find(actual.begin(), actual.end(), pos) == actual.end()) {
      result.falseNegatives++;
    }
  }

  if (result.truePositives + result.falsePositives > 0) {
    result.precision = static_cast<double>(result.truePositives) / (result.truePositives + result.falsePositives);
  }

  if (result.truePositives + result.falseNegatives > 0) {
    result.recall = static_cast<double>(result.truePositives) / (result.truePositives + result.falseNegatives);
  }

  if (result.precision + result.recall > 0) {
    result.f1Score = 2 * result.precision * result.recall / (result.precision + result.recall);
  }

  // Treat words with no expected and no actual hyphenation marks as perfect.
  if (expected.empty() && actual.empty()) {
    result.precision = 1.0;
    result.recall = 1.0;
    result.f1Score = 1.0;
  }

  double fpPenalty = 2.0;
  double fnPenalty = 1.0;
  int totalErrors = result.falsePositives * fpPenalty + result.falseNegatives * fnPenalty;
  int totalPossible = static_cast<int>(expected.size() * fpPenalty);

  if (totalPossible > 0) {
    result.weightedScore = 1.0 - (static_cast<double>(totalErrors) / totalPossible);
    result.weightedScore = std::max(0.0, result.weightedScore);
  } else if (result.falsePositives == 0) {
    result.weightedScore = 1.0;
  }

  return result;
}

// Runs the evaluation for a single language and asserts the per-word average F1
// is at or above `minF1Percent`. Thresholds are set ~1pp below measured
// baselines so unrelated tweaks don't fail CI but real regressions still trip.
void runLanguageEval(const char* langName, const char* primaryTag, const char* resourceFile, double minF1Percent) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag(primaryTag);
  ASSERT_NE(hyphenator, nullptr) << "No hyphenator registered for tag: " << primaryTag;

  std::string path = std::string(HYPHENATION_RESOURCES_DIR) + "/" + resourceFile;
  std::vector<TestCase> testCases = loadTestData(path);
  ASSERT_FALSE(testCases.empty()) << "No test cases loaded from " << path;

  double totalF1 = 0.0;
  std::vector<std::pair<TestCase, EvaluationResult>> imperfect;

  for (const auto& tc : testCases) {
    std::vector<size_t> actual = hyphenateWordWithHyphenator(tc.word, *hyphenator);
    EvaluationResult res = evaluateWord(tc, actual);
    totalF1 += res.f1Score;
    if (res.weightedScore < 0.999999) {
      imperfect.emplace_back(tc, res);
    }
  }

  double averageF1Percent = totalF1 / testCases.size() * 100.0;
  ::testing::Test::RecordProperty("avg_f1_percent", std::to_string(averageF1Percent));
  ::testing::Test::RecordProperty("test_cases", std::to_string(testCases.size()));

  std::cout << langName << ": F1=" << averageF1Percent << "% (threshold " << minF1Percent << "%, " << testCases.size()
            << " cases)\n";

  if (averageF1Percent < minF1Percent) {
    std::sort(imperfect.begin(), imperfect.end(),
              [](const auto& a, const auto& b) { return a.second.weightedScore < b.second.weightedScore; });
    std::cout << "Worst cases for " << langName << ":\n";
    int show = std::min<int>(10, static_cast<int>(imperfect.size()));
    for (int i = 0; i < show; ++i) {
      const TestCase& tc = imperfect[i].first;
      std::vector<size_t> actual = hyphenateWordWithHyphenator(tc.word, *hyphenator);
      std::cout << "  " << tc.word << " | expected=" << tc.hyphenated
                << " | got=" << positionsToHyphenated(tc.word, actual) << "\n";
    }
  }

  EXPECT_GE(averageF1Percent, minF1Percent) << "Hyphenation quality regressed for " << langName;
}

}  // namespace

TEST(HyphenationEval, English) { runLanguageEval("english", "en", "english_hyphenation_tests.txt", 98.10); }
TEST(HyphenationEval, French) { runLanguageEval("french", "fr", "french_hyphenation_tests.txt", 99.00); }
TEST(HyphenationEval, German) { runLanguageEval("german", "de", "german_hyphenation_tests.txt", 96.73); }
TEST(HyphenationEval, Russian) { runLanguageEval("russian", "ru", "russian_hyphenation_tests.txt", 96.22); }
TEST(HyphenationEval, Spanish) { runLanguageEval("spanish", "es", "spanish_hyphenation_tests.txt", 98.02); }
TEST(HyphenationEval, Italian) { runLanguageEval("italian", "it", "italian_hyphenation_tests.txt", 98.99); }
TEST(HyphenationEval, Polish) { runLanguageEval("polish", "pl", "polish_hyphenation_tests.txt", 98.92); }
TEST(HyphenationEval, Swedish) { runLanguageEval("swedish", "sv", "swedish_hyphenation_tests.txt", 94.01); }
