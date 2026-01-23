#include "FontManager.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cstring>

// Out-of-class definitions for static constexpr members (required for ODR-use
// in C++14)
constexpr int FontManager::MAX_FONTS;
constexpr const char *FontManager::FONTS_DIR;
constexpr const char *FontManager::SETTINGS_FILE;
constexpr uint8_t FontManager::SETTINGS_VERSION;

FontManager &FontManager::getInstance() {
  static FontManager instance;
  return instance;
}

void FontManager::scanFonts() {
  _fontCount = 0;

  FsFile dir = SdMan.open(FONTS_DIR, O_RDONLY);
  if (!dir) {
    Serial.printf("[FONT_MGR] Cannot open fonts directory: %s\n", FONTS_DIR);
    return;
  }

  if (!dir.isDir()) {
    Serial.printf("[FONT_MGR] %s is not a directory\n", FONTS_DIR);
    dir.close();
    return;
  }

  FsFile entry;
  while (_fontCount < MAX_FONTS && entry.openNext(&dir, O_RDONLY)) {
    if (entry.isDir()) {
      entry.close();
      continue;
    }

    char filename[64];
    entry.getName(filename, sizeof(filename));
    entry.close();

    // Check .bin extension
    if (!strstr(filename, ".bin")) {
      continue;
    }

    // Try to parse filename
    FontInfo &info = _fonts[_fontCount];
    strncpy(info.filename, filename, sizeof(info.filename) - 1);
    info.filename[sizeof(info.filename) - 1] = '\0';

    // Parse filename to get font info
    char nameCopy[64];
    strncpy(nameCopy, filename, sizeof(nameCopy) - 1);
    nameCopy[sizeof(nameCopy) - 1] = '\0';

    // Remove .bin
    char *ext = strstr(nameCopy, ".bin");
    if (ext)
      *ext = '\0';

    // Parse _WxH
    char *lastUnderscore = strrchr(nameCopy, '_');
    if (!lastUnderscore)
      continue;

    int w, h;
    if (sscanf(lastUnderscore + 1, "%dx%d", &w, &h) != 2)
      continue;
    info.width = (uint8_t)w;
    info.height = (uint8_t)h;
    *lastUnderscore = '\0';

    // Parse _size
    lastUnderscore = strrchr(nameCopy, '_');
    if (!lastUnderscore)
      continue;

    int size;
    if (sscanf(lastUnderscore + 1, "%d", &size) != 1)
      continue;
    info.size = (uint8_t)size;
    *lastUnderscore = '\0';

    // Font name
    strncpy(info.name, nameCopy, sizeof(info.name) - 1);
    info.name[sizeof(info.name) - 1] = '\0';

    Serial.printf("[FONT_MGR] Found font: %s (%dpt, %dx%d)\n", info.name,
                  info.size, info.width, info.height);

    _fontCount++;
  }

  dir.close();
  Serial.printf("[FONT_MGR] Scan complete: %d fonts found\n", _fontCount);
}

const FontInfo *FontManager::getFontInfo(int index) const {
  if (index < 0 || index >= _fontCount) {
    return nullptr;
  }
  return &_fonts[index];
}

bool FontManager::loadSelectedFont() {
  _activeFont.unload();

  if (_selectedIndex < 0 || _selectedIndex >= _fontCount) {
    return false;
  }

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR,
           _fonts[_selectedIndex].filename);

  return _activeFont.load(filepath);
}

bool FontManager::loadSelectedUiFont() {
  _activeUiFont.unload();

  if (_selectedUiIndex < 0 || _selectedUiIndex >= _fontCount) {
    return false;
  }

  char filepath[80];
  snprintf(filepath, sizeof(filepath), "%s/%s", FONTS_DIR,
           _fonts[_selectedUiIndex].filename);

  return _activeUiFont.load(filepath);
}

void FontManager::selectFont(int index) {
  if (index == _selectedIndex) {
    return;
  }

  _selectedIndex = index;

  if (index >= 0) {
    loadSelectedFont();
  } else {
    _activeFont.unload();
  }

  saveSettings();
}

void FontManager::selectUiFont(int index) {
  if (index == _selectedUiIndex) {
    return;
  }

  _selectedUiIndex = index;

  if (index >= 0) {
    loadSelectedUiFont();
  } else {
    _activeUiFont.unload();
  }

  saveSettings();
}

ExternalFont *FontManager::getActiveFont() {
  if (_selectedIndex >= 0 && _activeFont.isLoaded()) {
    return &_activeFont;
  }
  return nullptr;
}

ExternalFont *FontManager::getActiveUiFont() {
  if (_selectedUiIndex >= 0 && _activeUiFont.isLoaded()) {
    return &_activeUiFont;
  }
  return nullptr;
}

void FontManager::saveSettings() {
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("FONT_MGR", SETTINGS_FILE, file)) {
    Serial.printf("[FONT_MGR] Failed to save settings\n");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writePod(file, _selectedIndex);

  // Save selected reader font filename (for matching when restoring)
  if (_selectedIndex >= 0 && _selectedIndex < _fontCount) {
    serialization::writeString(file,
                               std::string(_fonts[_selectedIndex].filename));
  } else {
    serialization::writeString(file, std::string(""));
  }

  // Save UI font settings (version 2+)
  serialization::writePod(file, _selectedUiIndex);
  if (_selectedUiIndex >= 0 && _selectedUiIndex < _fontCount) {
    serialization::writeString(file,
                               std::string(_fonts[_selectedUiIndex].filename));
  } else {
    serialization::writeString(file, std::string(""));
  }

  file.close();
  Serial.printf("[FONT_MGR] Settings saved\n");
}

void FontManager::loadSettings() {
  FsFile file;
  if (!SdMan.openFileForRead("FONT_MGR", SETTINGS_FILE, file)) {
    Serial.printf("[FONT_MGR] No settings file, using defaults\n");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version < 1 || version > SETTINGS_VERSION) {
    Serial.printf("[FONT_MGR] Settings version mismatch (%d vs %d)\n", version,
                  SETTINGS_VERSION);
    file.close();
    return;
  }

  // Load reader font settings
  int savedIndex;
  serialization::readPod(file, savedIndex);

  std::string savedFilename;
  serialization::readString(file, savedFilename);

  // Find matching reader font by filename
  if (savedIndex >= 0 && !savedFilename.empty()) {
    for (int i = 0; i < _fontCount; i++) {
      if (savedFilename == _fonts[i].filename) {
        _selectedIndex = i;
        loadSelectedFont();
        Serial.printf("[FONT_MGR] Restored reader font: %s\n",
                      savedFilename.c_str());
        break;
      }
    }
    if (_selectedIndex < 0) {
      Serial.printf("[FONT_MGR] Saved reader font not found: %s\n",
                    savedFilename.c_str());
    }
  }

  // Load UI font settings (version 2+)
  if (version >= 2) {
    int savedUiIndex;
    serialization::readPod(file, savedUiIndex);

    std::string savedUiFilename;
    serialization::readString(file, savedUiFilename);

    if (savedUiIndex >= 0 && !savedUiFilename.empty()) {
      for (int i = 0; i < _fontCount; i++) {
        if (savedUiFilename == _fonts[i].filename) {
          _selectedUiIndex = i;
          loadSelectedUiFont();
          Serial.printf("[FONT_MGR] Restored UI font: %s\n",
                        savedUiFilename.c_str());
          break;
        }
      }
      if (_selectedUiIndex < 0) {
        Serial.printf("[FONT_MGR] Saved UI font not found: %s\n",
                      savedUiFilename.c_str());
      }
    }
  }

  file.close();
}
