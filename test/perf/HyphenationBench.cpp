#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/Hyphenator.h"

int main() {
  // Representative English words of varying length
  const std::vector<std::string> words = {
      "the",
      "beautiful",
      "international",
      "communication",
      "responsibility",
      "extraordinary",
      "understanding",
      "philosophical",
      "representative",
      "environmental",
      "administration",
      "comprehensive",
      "acknowledgment",
      "classification",
      "discrimination",
      "implementation",
      "infrastructure",
      "interpretation",
      "recommendation",
      "congratulations",
      "hello",
      "world",
      "computer",
      "programming",
      "architecture",
      "university",
      "mathematics",
      "information",
      "encyclopedia",
      "characterization",
  };

  Hyphenator::setPreferredLanguage("en");

  // Warm up
  for (const auto& w : words) {
    Hyphenator::breakOffsets(w, false);
  }

  const int iterations = 10000;
  const auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < iterations; ++i) {
    for (const auto& w : words) {
      Hyphenator::breakOffsets(w, false);
    }
  }

  const auto end = std::chrono::high_resolution_clock::now();
  const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  const long long totalCalls = static_cast<long long>(iterations) * static_cast<long long>(words.size());

  std::cout << "Hyphenation benchmark:\n";
  std::cout << "  Words:          " << words.size() << "\n";
  std::cout << "  Iterations:     " << iterations << "\n";
  std::cout << "  Total calls:    " << totalCalls << "\n";
  std::cout << "  Total time:     " << totalUs << " us\n";
  std::cout << "  Per-word avg:   " << (totalUs * 1000 / totalCalls) << " ns\n";

  return 0;
}
