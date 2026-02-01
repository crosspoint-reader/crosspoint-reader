#include "SleepBmpSelectionActivity.h"

#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "../../../lib/GfxRenderer/Bitmap.h"
#include "CrossPointSettings.h"

namespace {
void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    return std::lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return std::tolower(char1) < std::tolower(char2); });
  });
}
}  // namespace

SleepBmpSelectionActivity::SleepBmpSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::function<void()>& onBack)
    : ListSelectionActivity(
          "SleepBmpSelection", renderer, mappedInput, "Select Sleep BMP", [this]() { return files.size(); },
          [this](size_t index) { return files[index]; },
          [this, onBack](size_t index) {
            if (index >= files.size()) {
              return;
            }
            const std::string selectedFile = files[index];
            if (selectedFile == "Random") {
              // Clear the selection to use random
              SETTINGS.selectedSleepBmp[0] = '\0';
            } else {
              strncpy(SETTINGS.selectedSleepBmp, selectedFile.c_str(), sizeof(SETTINGS.selectedSleepBmp) - 1);
              SETTINGS.selectedSleepBmp[sizeof(SETTINGS.selectedSleepBmp) - 1] = '\0';
            }
            SETTINGS.saveToFile();
            onBack();
          },
          onBack, "No BMP files found in /sleep") {}

void SleepBmpSelectionActivity::loadFiles() {
  files.clear();

  std::vector<std::string> bmpFiles;

  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    char name[500];

    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }

      file.getName(name, sizeof(name));
      auto filename = std::string(name);

      if (filename[0] == '.' || filename.length() < 4 || filename.substr(filename.length() - 4) != ".bmp") {
        file.close();
        continue;
      }

      // Validate BMP
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        file.close();
        continue;
      }
      file.close();

      bmpFiles.emplace_back(filename);
    }
    dir.close();

    // Sort alphabetically (case-insensitive)
    sortFileList(bmpFiles);
  }

  // Add "Random" as first option, then sorted BMP files
  files.emplace_back("Random");
  files.insert(files.end(), bmpFiles.begin(), bmpFiles.end());
}

void SleepBmpSelectionActivity::loadItems() {
  loadFiles();

  // Set initial selection based on saved setting
  if (SETTINGS.selectedSleepBmp[0] == '\0') {
    selectorIndex = 0;  // "Random" is at index 0
  } else {
    // Find the selected file in the sorted list
    selectorIndex = 0;  // Default to "Random" if not found
    for (size_t i = 1; i < files.size(); i++) {
      if (files[i] == SETTINGS.selectedSleepBmp) {
        selectorIndex = i;
        break;
      }
    }
  }
}

void SleepBmpSelectionActivity::onExit() {
  ListSelectionActivity::onExit();
  files.clear();
}
