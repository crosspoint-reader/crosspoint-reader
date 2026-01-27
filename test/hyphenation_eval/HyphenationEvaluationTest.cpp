#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

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

struct LanguageConfig {
  std::string cliName;
  std::string testDataFile;
  const char* primaryTag;
};

const std::vector<LanguageConfig> kSupportedLanguages = {
    {"english", "test/hyphenation_eval/resources/english_hyphenation_tests.txt", "en"},
    {"french", "test/hyphenation_eval/resources/french_hyphenation_tests.txt", "fr"},
    {"german", "test/hyphenation_eval/resources/german_hyphenation_tests.txt", "de"},
    {"russian", "test/hyphenation_eval/resources/russian_hyphenation_tests.txt", "ru"},
    {"spanish", "test/hyphenation_eval/resources/spanish_hyphenation_tests.txt", "es"},
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
    std::cerr << "Error: Could not open file " << filename << std::endl;
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

  file.close();
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

std::vector<LanguageConfig> resolveLanguages(const std::string& selection) {
  if (selection == "all") {
    return kSupportedLanguages;
  }

  for (const auto& config : kSupportedLanguages) {
    if (config.cliName == selection) {
      return {config};
    }
  }

  return {};
}

EvaluationResult evaluateWord(const TestCase& testCase,
                              std::function<std::vector<size_t>(const std::string&)> hyphenateFunc) {
  EvaluationResult result;

  std::vector<size_t> actualPositions = hyphenateFunc(testCase.word);

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

  // Treat words that contain no hyphenation marks in both the expected data and the
  // algorithmic output as perfect matches so they don't drag down the per-word averages.
  if (expected.empty() && actual.empty()) {
    result.precision = 1.0;
    result.recall = 1.0;
    result.f1Score = 1.0;
  }

  double fpPenalty = 2.0;
  double fnPenalty = 1.0;

  int totalErrors = result.falsePositives * fpPenalty + result.falseNegatives * fnPenalty;
  int totalPossible = expected.size() * fpPenalty;

  if (totalPossible > 0) {
    result.weightedScore = 1.0 - (static_cast<double>(totalErrors) / totalPossible);
    result.weightedScore = std::max(0.0, result.weightedScore);
  } else if (result.falsePositives == 0) {
    result.weightedScore = 1.0;
  }

  return result;
}

void printResults(const std::string& language, const std::vector<TestCase>& testCases,
                  const std::vector<std::pair<TestCase, EvaluationResult>>& worstCases, int perfectMatches,
                  int partialMatches, int completeMisses, double totalPrecision, double totalRecall, double totalF1,
                  double totalWeighted, int totalTP, int totalFP, int totalFN,
                  std::function<std::vector<size_t>(const std::string&)> hyphenateFunc) {
  std::string lang_upper = language;
  if (!lang_upper.empty()) {
    lang_upper[0] = std::toupper(lang_upper[0]);
  }

  std::cout << "================================================================================" << std::endl;
  std::cout << lang_upper << " HYPHENATION EVALUATION RESULTS" << std::endl;
  std::cout << "================================================================================" << std::endl;
  std::cout << std::endl;

  std::cout << "Total test cases:   " << testCases.size() << std::endl;
  std::cout << "Perfect matches:    " << perfectMatches << " (" << (perfectMatches * 100.0 / testCases.size()) << "%)"
            << std::endl;
  std::cout << "Partial matches:    " << partialMatches << std::endl;
  std::cout << "Complete misses:    " << completeMisses << std::endl;
  std::cout << std::endl;

  std::cout << "--- Overall Metrics (averaged per word) ---" << std::endl;
  std::cout << "Average Precision:       " << (totalPrecision / testCases.size() * 100.0) << "%" << std::endl;
  std::cout << "Average Recall:          " << (totalRecall / testCases.size() * 100.0) << "%" << std::endl;
  std::cout << "Average F1 Score:        " << (totalF1 / testCases.size() * 100.0) << "%" << std::endl;
  std::cout << "Average Weighted Score:  " << (totalWeighted / testCases.size() * 100.0) << "% (FP penalty: 2x)"
            << std::endl;
  std::cout << std::endl;

  std::cout << "--- Overall Metrics (total counts) ---" << std::endl;
  std::cout << "True Positives:          " << totalTP << std::endl;
  std::cout << "False Positives:         " << totalFP << " (incorrect hyphenation points)" << std::endl;
  std::cout << "False Negatives:         " << totalFN << " (missed hyphenation points)" << std::endl;

  double overallPrecision = totalTP + totalFP > 0 ? static_cast<double>(totalTP) / (totalTP + totalFP) : 0.0;
  double overallRecall = totalTP + totalFN > 0 ? static_cast<double>(totalTP) / (totalTP + totalFN) : 0.0;
  double overallF1 = overallPrecision + overallRecall > 0
                         ? 2 * overallPrecision * overallRecall / (overallPrecision + overallRecall)
                         : 0.0;

  std::cout << "Overall Precision:       " << (overallPrecision * 100.0) << "%" << std::endl;
  std::cout << "Overall Recall:          " << (overallRecall * 100.0) << "%" << std::endl;
  std::cout << "Overall F1 Score:        " << (overallF1 * 100.0) << "%" << std::endl;
  std::cout << std::endl;

  // Filter out perfect matches from the “worst cases” section so that only actionable failures appear.
  auto hasImperfection = [](const EvaluationResult& r) { return r.weightedScore < 0.999999; };
  std::vector<std::pair<TestCase, EvaluationResult>> imperfectCases;
  imperfectCases.reserve(worstCases.size());
  for (const auto& entry : worstCases) {
    if (hasImperfection(entry.second)) {
      imperfectCases.push_back(entry);
    }
  }

  std::cout << "--- Worst Cases (lowest weighted scores) ---" << std::endl;
  int showCount = std::min(10, static_cast<int>(imperfectCases.size()));
  for (int i = 0; i < showCount; i++) {
    const auto& testCase = imperfectCases[i].first;
    const auto& result = imperfectCases[i].second;

    std::vector<size_t> actualPositions = hyphenateFunc(testCase.word);
    std::string actualHyphenated = positionsToHyphenated(testCase.word, actualPositions);

    std::cout << "Word: " << testCase.word << " (freq: " << testCase.frequency << ")" << std::endl;
    std::cout << "  Expected:  " << testCase.hyphenated << std::endl;
    std::cout << "  Got:       " << actualHyphenated << std::endl;
    std::cout << "  Precision: " << (result.precision * 100.0) << "%"
              << "  Recall: " << (result.recall * 100.0) << "%"
              << "  F1: " << (result.f1Score * 100.0) << "%"
              << "  Weighted: " << (result.weightedScore * 100.0) << "%" << std::endl;
    std::cout << "  TP: " << result.truePositives << "  FP: " << result.falsePositives
              << "  FN: " << result.falseNegatives << std::endl;
    std::cout << std::endl;
  }

  // Additional compact list of the worst ~100 words to aid iteration
  int compactCount = std::min(100, static_cast<int>(imperfectCases.size()));
  if (compactCount > 0) {
    std::cout << "--- Compact Worst Cases (" << compactCount << ") ---" << std::endl;
    for (int i = 0; i < compactCount; i++) {
      const auto& testCase = imperfectCases[i].first;
      std::vector<size_t> actualPositions = hyphenateFunc(testCase.word);
      std::string actualHyphenated = positionsToHyphenated(testCase.word, actualPositions);
      std::cout << testCase.word << " | exp:" << testCase.hyphenated << " | got:" << actualHyphenated << std::endl;
    }
    std::cout << std::endl;
  }
}

int main(int argc, char* argv[]) {
  const bool summaryMode = argc <= 1;
  const std::string languageSelection = summaryMode ? "all" : argv[1];

  std::vector<LanguageConfig> languages = resolveLanguages(languageSelection);
  if (languages.empty()) {
    std::cerr << "Unknown language: " << languageSelection << std::endl;
    return 1;
  }

  for (const auto& lang : languages) {
    const auto* hyphenator = getLanguageHyphenatorForPrimaryTag(lang.primaryTag);
    if (!hyphenator) {
      std::cerr << "No hyphenator registered for tag: " << lang.primaryTag << std::endl;
      continue;
    }
    const auto hyphenateFunc = [hyphenator](const std::string& word) {
      return hyphenateWordWithHyphenator(word, *hyphenator);
    };

    if (!summaryMode) {
      std::cout << "Loading test data from: " << lang.testDataFile << std::endl;
    }
    std::vector<TestCase> testCases = loadTestData(lang.testDataFile);

    if (testCases.empty()) {
      std::cerr << "No test cases loaded for " << lang.cliName << ". Skipping." << std::endl;
      continue;
    }

    if (!summaryMode) {
      std::cout << "Loaded " << testCases.size() << " test cases for " << lang.cliName << std::endl;
      std::cout << std::endl;
    }

    int perfectMatches = 0;
    int partialMatches = 0;
    int completeMisses = 0;

    double totalPrecision = 0.0;
    double totalRecall = 0.0;
    double totalF1 = 0.0;
    double totalWeighted = 0.0;

    int totalTP = 0, totalFP = 0, totalFN = 0;

    std::vector<std::pair<TestCase, EvaluationResult>> worstCases;

    for (const auto& testCase : testCases) {
      EvaluationResult result = evaluateWord(testCase, hyphenateFunc);

      totalTP += result.truePositives;
      totalFP += result.falsePositives;
      totalFN += result.falseNegatives;

      totalPrecision += result.precision;
      totalRecall += result.recall;
      totalF1 += result.f1Score;
      totalWeighted += result.weightedScore;

      if (result.f1Score == 1.0) {
        perfectMatches++;
      } else if (result.f1Score > 0.0) {
        partialMatches++;
      } else {
        completeMisses++;
      }

      worstCases.push_back({testCase, result});
    }

    if (summaryMode) {
      const double averageF1Percent = testCases.empty() ? 0.0 : (totalF1 / testCases.size() * 100.0);
      std::cout << lang.cliName << ": " << averageF1Percent << "%" << std::endl;
      continue;
    }

    std::sort(worstCases.begin(), worstCases.end(),
              [](const auto& a, const auto& b) { return a.second.weightedScore < b.second.weightedScore; });

    printResults(lang.cliName, testCases, worstCases, perfectMatches, partialMatches, completeMisses, totalPrecision,
                 totalRecall, totalF1, totalWeighted, totalTP, totalFP, totalFN, hyphenateFunc);
  }

  return 0;
}