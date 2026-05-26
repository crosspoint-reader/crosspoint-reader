#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Epub;

namespace BookSyntheticIndex {

struct BuiltIndex {
  uint32_t charsPerPage = 0;
  uint32_t totalPages = 0;
  uint32_t totalTextCodepoints = 0;
  std::vector<uint32_t> pageStartChar;
  /** 1-based synthetic page index at each spine start; v4+ cache; aligns with KOReader spine boundaries. */
  std::vector<uint32_t> spineFirstSyntheticPage;
};

using BuildProgressFn = std::function<void(int completedSpines, int spineCount)>;

bool loadFromCache(const std::string& cachePath, int spineCount, uint32_t cpp, BuiltIndex& out);
bool buildAndSave(const Epub& epub, uint32_t cpp, BuiltIndex& out, BuildProgressFn onProgress = {});

}  // namespace BookSyntheticIndex
