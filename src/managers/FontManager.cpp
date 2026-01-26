#include "FontManager.h"

#include <GfxRenderer.h>  // for EpdFontData usage validation if needed
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <algorithm>

#include "CustomEpdFont.h"

FontManager& FontManager::getInstance() {
  static FontManager instance;
  return instance;
}

FontManager::~FontManager() {
  for (auto& familyPair : loadedFonts) {
    for (auto& sizePair : familyPair.second) {
      delete sizePair.second;
    }
  }
}

const std::vector<std::string>& FontManager::getAvailableFamilies() {
  if (!scanned) {
    scanFonts();
  }
  return availableFamilies;
}

void FontManager::scanFonts() {
  Serial.println("[FM] Scanning fonts...");
  availableFamilies.clear();
  scanned = true;

  FsFile fontDir;
  if (!SdMan.openFileForRead("FontScan", "/fonts", fontDir)) {
    Serial.println("[FM] Failed to open /fonts directory");
    // Even if failed, we proceed to sort empty list to avoid crashes
    return;
  }

  if (!fontDir.isDirectory()) {
    Serial.println("[FM] /fonts is not a directory");
    fontDir.close();
    return;
  }

  Serial.println("[FM] /fonts opened. Iterating files...");
  FsFile file;
  while (file.openNext(&fontDir, O_READ)) {
    if (!file.isDirectory()) {
      char filename[128];
      file.getName(filename, sizeof(filename));
      Serial.printf("[FM] Checking: %s\n", filename);

      String name = String(filename);
      if (name.endsWith(".epdfont")) {
        // Expected format: Family-Style-Size.epdfont or Family_Size.epdfont
        // Or just Family.epdfont (V1 single file)

        String family;
        int separator = name.indexOf('-');
        if (separator < 0) separator = name.indexOf('_');

        if (separator > 0) {
          family = name.substring(0, separator);
        } else {
          // No separator, take the whole name (minus .epdfont)
          family = name.substring(0, name.length() - 8);
        }

        if (family.length() > 0) {
          if (std::find(availableFamilies.begin(), availableFamilies.end(), family.c_str()) ==
              availableFamilies.end()) {
            availableFamilies.push_back(family.c_str());
            Serial.printf("[FM] Added family: %s\n", family.c_str());
          }
        }
      }
    }
    file.close();
  }
  fontDir.close();

  std::sort(availableFamilies.begin(), availableFamilies.end());
  Serial.printf("[FM] Scan complete. Found %d families\n", availableFamilies.size());
}

struct EpdfHeader {
  char magic[4];
  uint32_t intervalCount;
  uint32_t totalGlyphCount;
  uint8_t advanceY;
  int32_t ascender;
  int32_t descender;
  uint8_t is2Bit;
};

// Helper to load a single font file
CustomEpdFont* loadFontFile(const String& path) {
  Serial.printf("[FontMgr] Loading file: %s\n", path.c_str());
  Serial.flush();
  FsFile f;
  if (!SdMan.openFileForRead("FontLoading", path.c_str(), f)) {
    Serial.printf("[FontMgr] Failed to open: %s\n", path.c_str());
    Serial.flush();
    return nullptr;
  }

  // Read custom header format (detected from file dump)
  // 0: Magic (4)
  // 4: IntervalCount (4)
  // 8: FileSize (4)
  // 12: Height (4) -> advanceY
  // 16: GlyphCount (4)
  // 20: Ascender (4)
  // 24: Unknown (4)
  // 28: Descender (4)
  // 32: Unknown (4)
  // 36: OffsetIntervals (4)
  // 40: OffsetGlyphs (4)
  // 44: OffsetBitmaps (4)

  uint32_t buf[12];  // 48 bytes
  if (f.read(buf, 48) != 48) {
    Serial.printf("[FontMgr] Header read failed for %s\n", path.c_str());
    f.close();
    return nullptr;
  }

  if (strncmp((char*)&buf[0], "EPDF", 4) != 0) {
    Serial.printf("[FontMgr] Invalid magic for %s\n", path.c_str());
    f.close();
    return nullptr;
  }

  Serial.printf("[FontMgr] Header Dump %s: ", path.c_str());
  for (int i = 0; i < 12; i++) Serial.printf("%08X ", buf[i]);
  Serial.println();

  /*
   * Version Detection Improved
   *
   * V1:
   *   Offset 20 (buf[5]) is OffsetIntervals. It matches header size = 32.
   *   Offset 4 (buf[1]) low 16 bits is Version = 1.
   *
   * V0:
   *   Offset 36 (buf[9]) is OffsetIntervals. It matches header size = 48.
   */

  int version = -1;

  // Check for V1
  if (buf[5] == 32 && (buf[1] & 0xFFFF) == 1) {
    version = 1;
  }
  // Check for V0
  else if (buf[9] == 48) {
    version = 0;
  }
  // Fallback: Use the old file size check if offsets are weird (detected from legacy files?)
  else if (buf[2] > 10000) {
    // V0 has fileSize at offset 8 (buf[2])
    version = 0;
  }

  uint32_t intervalCount, fileSize, glyphCount, offsetIntervals, offsetGlyphs, offsetBitmaps;
  uint8_t height, advanceY;
  int32_t ascender, descender;
  bool is2Bit;

  if (version == 1) {
    // V1 Parsing
    uint8_t* b8 = (uint8_t*)buf;

    is2Bit = (b8[6] != 0);
    advanceY = b8[8];
    ascender = (int8_t)b8[9];
    descender = (int8_t)b8[10];

    intervalCount = b8[12] | (b8[13] << 8) | (b8[14] << 16) | (b8[15] << 24);
    glyphCount = b8[16] | (b8[17] << 8) | (b8[18] << 16) | (b8[19] << 24);
    offsetIntervals = b8[20] | (b8[21] << 8) | (b8[22] << 16) | (b8[23] << 24);
    offsetGlyphs = b8[24] | (b8[25] << 8) | (b8[26] << 16) | (b8[27] << 24);
    offsetBitmaps = b8[28] | (b8[29] << 8) | (b8[30] << 16) | (b8[31] << 24);

    height = advanceY;
    fileSize = 0;  // Unknown

  } else if (version == 0) {
    // V0 Parsing
    // We already read 48 bytes into buf
    intervalCount = buf[1];
    fileSize = buf[2];
    height = buf[3];
    glyphCount = buf[4];
    ascender = (int32_t)buf[5];
    descender = (int32_t)buf[7];
    is2Bit = (buf[8] != 0);

    offsetIntervals = buf[9];
    offsetGlyphs = buf[10];
    offsetBitmaps = buf[11];
  } else {
    Serial.printf("[FontMgr] Unknown version for %s\n", path.c_str());
    f.close();
    return nullptr;
  }

  // Validation
  // For V1, we trust offsets generated by the tool
  if (offsetIntervals == 0 || offsetGlyphs == 0 || offsetBitmaps == 0) {
    Serial.println("[FontMgr] Invalid offsets in header");
    f.close();
    return nullptr;
  }

  // We need to load intervals into RAM
  EpdUnicodeInterval* intervals = new (std::nothrow) EpdUnicodeInterval[intervalCount];
  if (!intervals) {
    Serial.printf("[FontMgr] Failed to allocate intervals: %d\n", intervalCount);
    f.close();
    return nullptr;
  }

  if (!f.seekSet(offsetIntervals)) {
    Serial.println("[FontMgr] Failed to seek to intervals");
    delete[] intervals;
    f.close();
    return nullptr;
  }

  f.read((uint8_t*)intervals, intervalCount * sizeof(EpdUnicodeInterval));

  f.close();

  // Create EpdFontData
  EpdFontData* fontData = new (std::nothrow) EpdFontData();
  if (!fontData) {
    Serial.println("[FontMgr] Failed to allocate EpdFontData! OOM.");
    delete[] intervals;
    return nullptr;
  }
  fontData->intervalCount = intervalCount;
  fontData->intervals = intervals;
  fontData->glyph = nullptr;
  fontData->advanceY = (uint8_t)height;
  fontData->ascender = ascender;
  fontData->descender = descender;
  fontData->is2Bit = is2Bit;
  fontData->bitmap = nullptr;

  return new CustomEpdFont(path, fontData, offsetIntervals, offsetGlyphs, offsetBitmaps, version);
}

EpdFontFamily* FontManager::getCustomFontFamily(const std::string& familyName, int fontSize) {
  if (loadedFonts[familyName][fontSize]) {
    return loadedFonts[familyName][fontSize];
  }

  String basePath = "/fonts/" + String(familyName.c_str()) + "-";
  String sizeStr = String(fontSize);

  CustomEpdFont* regular = loadFontFile(basePath + "Regular-" + sizeStr + ".epdfont");

  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Regular_" + sizeStr + ".epdfont");
  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + "_" + sizeStr + ".epdfont");
  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + ".epdfont");
  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + "-Regular.epdfont");
  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Regular.epdfont");
  if (!regular) regular = loadFontFile("/fonts/" + String(familyName.c_str()) + "-" + sizeStr + ".epdfont");

  CustomEpdFont* bold = loadFontFile(basePath + "Bold-" + sizeStr + ".epdfont");
  if (!bold) bold = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Bold_" + sizeStr + ".epdfont");
  if (!bold) bold = loadFontFile("/fonts/" + String(familyName.c_str()) + "-Bold.epdfont");
  if (!bold) bold = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Bold.epdfont");

  CustomEpdFont* italic = loadFontFile(basePath + "Italic-" + sizeStr + ".epdfont");
  if (!italic) italic = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Italic_" + sizeStr + ".epdfont");
  if (!italic) italic = loadFontFile("/fonts/" + String(familyName.c_str()) + "-Italic.epdfont");
  if (!italic) italic = loadFontFile("/fonts/" + String(familyName.c_str()) + "_Italic.epdfont");

  CustomEpdFont* boldItalic = loadFontFile(basePath + "BoldItalic-" + sizeStr + ".epdfont");
  if (!boldItalic)
    boldItalic = loadFontFile("/fonts/" + String(familyName.c_str()) + "_BoldItalic_" + sizeStr + ".epdfont");
  if (!boldItalic) boldItalic = loadFontFile("/fonts/" + String(familyName.c_str()) + "-BoldItalic.epdfont");
  if (!boldItalic) boldItalic = loadFontFile("/fonts/" + String(familyName.c_str()) + "_BoldItalic.epdfont");

  if (!regular) {
    if (bold) regular = bold;
  }

  if (regular) {
    EpdFontFamily* family = new EpdFontFamily(regular, bold, italic, boldItalic);
    loadedFonts[familyName][fontSize] = family;
    return family;
  }

  return nullptr;
}
