#include "EpdFontLoader.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/CrossPointSettings.h"
#include "../../src/managers/FontManager.h"

std::vector<int> EpdFontLoader::loadedCustomIds;

void EpdFontLoader::loadFontsFromSd(GfxRenderer& renderer) {
  loadedCustomIds.clear();

  // Check settings for custom font
  if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
    if (strlen(SETTINGS.customFontFamily) > 0) {
      Serial.printf("Loading custom font: %s size %d\n", SETTINGS.customFontFamily, SETTINGS.fontSize);
      Serial.flush();

      // Map enum size to point size roughly (or use customFontSize if non-zero)
      int size = 12;  // default
      // Map generic sizes (Small, Medium, Large, XL) to likely point sizes if not specified
      // Assume standard sizes: 12, 14, 16, 18
      switch (SETTINGS.fontSize) {
        case CrossPointSettings::SMALL:
          size = 12;
          break;
        case CrossPointSettings::MEDIUM:
          size = 14;
          break;
        case CrossPointSettings::LARGE:
          size = 16;
          break;
        case CrossPointSettings::EXTRA_LARGE:
          size = 18;
          break;
      }

      EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(SETTINGS.customFontFamily, size);
      if (family) {
        // IDs are usually static consts. For custom font, we need a dynamic ID or reserved ID.
        // In main.cpp or somewhere, a range might be reserved or we replace an existing one?
        // The stash code in main.cpp step 120 showed:
        // "Calculate hash ID manually... int id = (int)hash;"
        // "renderer.insertFont(id, *msgFont);"

        std::string key = std::string(SETTINGS.customFontFamily) + "-" + std::to_string(size);
        uint32_t hash = 5381;
        for (char c : key) hash = ((hash << 5) + hash) + c;
        int id = (int)hash;

        Serial.printf("[FontLoader] Inserting custom font '%s' with ID %d (key: %s)\n", SETTINGS.customFontFamily, id,
                      key.c_str());
        renderer.insertFont(id, *family);
        loadedCustomIds.push_back(id);
      } else {
        Serial.println("Failed to load custom font family");
      }
    }
  }
}

int EpdFontLoader::getBestFontId(const char* familyName, int size) {
  if (!familyName || strlen(familyName) == 0) return -1;

  // For now, just return the deterministic hash.
  std::string key = std::string(familyName) + "-" + std::to_string(size);
  uint32_t hash = 5381;
  for (char c : key) hash = ((hash << 5) + hash) + c;
  int id = (int)hash;

  // Verify if the font was actually loaded
  bool found = false;
  for (int loadedId : loadedCustomIds) {
    if (loadedId == id) {
      found = true;
      break;
    }
  }

  if (found) {
    return id;
  } else {
    return -1;  // Fallback to builtin font
  }
}
