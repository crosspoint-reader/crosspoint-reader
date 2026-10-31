#include "ReaderFontSizes.h"

#include <algorithm>
#include <iterator>

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      if (family->vector) {
        return {std::begin(VECTOR_READER_POINT_SIZES), std::end(VECTOR_READER_POINT_SIZES)};
      }
      auto sizes = family->availableSizes();
      if (!sizes.empty()) return sizes;
    }
  }
  return {std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES)};
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pt) {
  if (!sizes || count == 0) return pt;

  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (size_t i = 1; i < count; i++) {
    const uint8_t delta = sizes[i] > pt ? sizes[i] - pt : pt - sizes[i];
    // Strictly-less keeps the smaller size on a tie, since `sizes` is ascending.
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}

int readerFontFamilyIndex(const SdCardFontRegistry* registry, const char* sdFamilyName, const uint8_t fontFamily) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    const auto& families = registry->getFamilies();
    const auto family = std::find_if(families.begin(), families.end(),
                                     [sdFamilyName](const auto& candidate) { return candidate.name == sdFamilyName; });
    if (family != families.end()) {
      return BUILTIN_READER_FONT_FAMILIES + static_cast<int>(family - families.begin());
    }
  }
  return fontFamily < BUILTIN_READER_FONT_FAMILIES ? fontFamily : 0;
}

const char* readerFontFamilySdName(const SdCardFontRegistry* registry, const int index) {
  if (index >= 0 && index < BUILTIN_READER_FONT_FAMILIES) return "";
  const int sdIndex = index - BUILTIN_READER_FONT_FAMILIES;
  if (!registry || sdIndex < 0 || sdIndex >= registry->getFamilyCount()) return nullptr;
  return registry->getFamilies()[sdIndex].name.c_str();
}
