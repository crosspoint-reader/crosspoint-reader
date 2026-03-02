#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();

  // Load all size/style variants for a discovered family.
  // Returns true if at least one font was loaded.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for a loaded family + size + style.
  // Returns 0 if not found.
  int getFontId(const std::string& familyName, uint8_t size, uint8_t style = 0) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; }

  // Get line compression for SD card fonts (same as NotoSans values).
  static float getLineCompression(uint8_t lineSpacing);

  // Generate deterministic font ID from family name + size + style.
  static int generateFontId(const std::string& name, uint8_t size, uint8_t style);

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
    uint8_t style;
  };
  std::string loadedFamilyName_;
  std::vector<LoadedFont> loaded_;
};
