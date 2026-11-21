#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
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

struct TestDataset {
  std::vector<TestCase> cases;
  size_t minPrefix = 0;
  size_t minSuffix = 0;
};

TestDataset loadTestData(const std::string& filename) {
  TestDataset dataset;
  std::ifstream file(filename);

  if (!file.is_open()) {
    return dataset;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      constexpr const char* kMinPrefixHeader = "# Min prefix:";
      constexpr const char* kMinSuffixHeader = "# Min suffix:";
      if (line.rfind(kMinPrefixHeader, 0) == 0) {
        dataset.minPrefix = static_cast<size_t>(std::stoul(line.substr(std::strlen(kMinPrefixHeader))));
      } else if (line.rfind(kMinSuffixHeader, 0) == 0) {
        dataset.minSuffix = static_cast<size_t>(std::stoul(line.substr(std::strlen(kMinSuffixHeader))));
      }
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
      dataset.cases.push_back(testCase);
    }
  }

  return dataset;
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

std::vector<size_t> hyphenateWordWithHyphenator(const std::string& word, const LanguageHyphenator& hyphenator,
                                                const size_t datasetMinPrefix = 0, const size_t datasetMinSuffix = 0) {
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);
  auto positions = hyphenator.breakIndexes(cps);
  if (datasetMinPrefix > 0 || datasetMinSuffix > 0) {
    const size_t len = cps.size();
    positions.erase(
        std::remove_if(positions.begin(), positions.end(),
                       [&](const size_t pos) { return pos < datasetMinPrefix || pos + datasetMinSuffix > len; }),
        positions.end());
  }
  return positions;
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
  TestDataset dataset = loadTestData(path);
  const std::vector<TestCase>& testCases = dataset.cases;
  ASSERT_FALSE(testCases.empty()) << "No test cases loaded from " << path;

  double totalF1 = 0.0;
  std::vector<std::pair<TestCase, EvaluationResult>> imperfect;

  for (const auto& tc : testCases) {
    std::vector<size_t> actual =
        hyphenateWordWithHyphenator(tc.word, *hyphenator, dataset.minPrefix, dataset.minSuffix);
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
      std::vector<size_t> actual =
          hyphenateWordWithHyphenator(tc.word, *hyphenator, dataset.minPrefix, dataset.minSuffix);
      std::cout << "  " << tc.word << " | expected=" << tc.hyphenated
                << " | got=" << positionsToHyphenated(tc.word, actual) << "\n";
    }
  }

  EXPECT_GE(averageF1Percent, minF1Percent) << "Hyphenation quality regressed for " << langName;
}

}  // namespace

TEST(HyphenationEval, English) { runLanguageEval("english", "en", "english_hyphenation_tests.txt", 98.10); }

TEST(HyphenationEval, EnglishUsesStandardTwoThreeBounds) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("en");
  ASSERT_NE(hyphenator, nullptr);
  EXPECT_EQ(hyphenator->minPrefix(), 2u);
  EXPECT_EQ(hyphenator->minSuffix(), 3u);

  const auto hyphenate = [&](const char* word) {
    return positionsToHyphenated(word, hyphenateWordWithHyphenator(word, *hyphenator));
  };
  EXPECT_EQ(hyphenate("Detroit"), "De=troit");
  EXPECT_EQ(hyphenate("mobile"), "mo=bile");
  EXPECT_EQ(hyphenate("devices"), "de=vices");
  EXPECT_EQ(hyphenate("expertise"), "ex=per=tise");
  EXPECT_EQ(hyphenate("entrepreneur").substr(0, 3), "en=");
  // 1-letter prefixes and 2-letter suffixes remain suppressed under 2/3 rules.
  EXPECT_EQ(hyphenate("around"), "around");
  EXPECT_EQ(hyphenate("walked"), "walked");
}
TEST(HyphenationEval, French) { runLanguageEval("french", "fr", "french_hyphenation_tests.txt", 99.00); }
TEST(HyphenationEval, German) { runLanguageEval("german", "de", "german_hyphenation_tests.txt", 96.73); }
TEST(HyphenationEval, Russian) { runLanguageEval("russian", "ru", "russian_hyphenation_tests.txt", 96.22); }
TEST(HyphenationEval, Spanish) { runLanguageEval("spanish", "es", "spanish_hyphenation_tests.txt", 98.02); }
TEST(HyphenationEval, Italian) { runLanguageEval("italian", "it", "italian_hyphenation_tests.txt", 98.99); }
TEST(HyphenationEval, Polish) { runLanguageEval("polish", "pl", "polish_hyphenation_tests.txt", 98.92); }
TEST(HyphenationEval, Swedish) { runLanguageEval("swedish", "sv", "swedish_hyphenation_tests.txt", 94.01); }
TEST(HyphenationEval, Portuguese) { runLanguageEval("portuguese", "pt", "portuguese_hyphenation_tests.txt", 98.21); }
