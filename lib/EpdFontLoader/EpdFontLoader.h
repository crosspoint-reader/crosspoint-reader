#pragma once

#include <GfxRenderer.h>

#include <vector>

class EpdFontLoader {
 public:
  static void loadFontsFromSd(GfxRenderer& renderer);
  static int getBestFontId(const char* familyName, int size);

 private:
  static std::vector<int> loadedCustomIds;
};
