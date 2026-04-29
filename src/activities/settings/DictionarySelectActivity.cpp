#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "DictPrepareActivity.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Candidate SD card root directories for dictionaries, checked in priority order.
// The first directory found on the SD card is used; the rest are ignored.
static constexpr const char* DICT_ROOT_CANDIDATES[] = {
    "/.dictionaries",
    "/dictionaries",
};

// Long press threshold for viewing dictionary metadata.
static constexpr unsigned long VIEW_INFO_MS = 1000;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();

  // Suppress Confirm bleed-through only in settings mode: when launched from a list,
  // the Confirm release that opened the picker fires again in the picker's first loop.
  // In per-book mode (launched via reader menu) the menu already consumed the event.
  ignoreNextConfirmRelease = bookCachePath.empty();

  scanDictionaries();

  if (bookCachePath.empty()) {
    // Settings mode: validate global path, pre-select from SETTINGS.
    Dictionary::isValidDictionary();

    selectedIndex = 0;  // default: None
    {
      const std::string activePath = Dictionary::readDictPath();
      if (!activePath.empty()) {
        for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
          if (folderForIndex(i + 1) == activePath) {
            selectedIndex = i + 1;
            break;
          }
        }
      }
    }
  } else {
    // Per-book mode: read saved per-book path, pre-select it.
    currentBookDictPath = "";
    FsFile f;
    if (Storage.openFileForRead("DSEL", bookCachePath + "/dictionary.bin", f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
        std::string path(sz, '\0');
        const int n = f.read(&path[0], sz);
        if (n > 0) {
          path.resize(n);
          currentBookDictPath = path;
        }
      }
      f.close();
    }

    selectedIndex = 0;  // default: Use Global
    if (!currentBookDictPath.empty()) {
      for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
        if (folderForIndex(i + 1) == currentBookDictPath) {
          selectedIndex = i + 1;
          break;
        }
      }
    }

    // Build augmented "Use Global" label showing the active global dictionary name.
    // Path format: <dictRoot>/<folder>/<stem> — extract <folder>.
    const std::string globalPath = Dictionary::readDictPath();
    std::string globalFolderName;
    if (globalPath.empty()) {
      globalFolderName = tr(STR_DICT_NONE);
    } else {
      const size_t lastSlash = globalPath.rfind('/');
      if (lastSlash != std::string::npos && lastSlash > 0) {
        const size_t prevSlash = globalPath.rfind('/', lastSlash - 1);
        globalFolderName = (prevSlash != std::string::npos)
                               ? globalPath.substr(prevSlash + 1, lastSlash - prevSlash - 1)
                               : globalPath.substr(0, lastSlash);
      } else {
        globalFolderName = globalPath;
      }
    }
    useGlobalLabel = std::string(tr(STR_DICT_USE_GLOBAL)) + " (" + globalFolderName + ")";
  }

  totalItems = 1 + static_cast<int>(dictFolders.size());
  showingInfo = false;

  requestUpdate();
}

void DictionarySelectActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
// SD card scan
// ---------------------------------------------------------------------------

void DictionarySelectActivity::scanDictionaries() {
  dictFolders.clear();
  dictFolders.reserve(16);
  dictStems.clear();
  dictStems.reserve(16);
  dictRoot.clear();

  for (const auto* candidate : DICT_ROOT_CANDIDATES) {
    auto dir = Storage.open(candidate);
    if (dir && dir.isDirectory()) {
      dictRoot = candidate;
      dir.close();
      break;
    }
    if (dir) dir.close();
  }

  if (dictRoot.empty()) {
    LOG_DBG("DSEL", "No dictionary directory found on SD card");
    return;
  }

  auto root = Storage.open(dictRoot.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));

    if (!entry.isDirectory() || name[0] == '.') {
      entry.close();
      continue;
    }

    // Scan the subdirectory for .idx and .ifo files.
    // Folders with multiple .idx or multiple .ifo files are ambiguous and skipped.
    std::string subPath = dictRoot + "/" + name;
    entry.close();

    auto subDir = Storage.open(subPath.c_str());
    if (!subDir || !subDir.isDirectory()) {
      if (subDir) subDir.close();
      continue;
    }

    subDir.rewindDirectory();
    char subName[500];
    char foundStem[500];
    foundStem[0] = '\0';
    bool ambiguous = false;
    int ifoCount = 0;
    for (auto subEntry = subDir.openNextFile(); subEntry; subEntry = subDir.openNextFile()) {
      subEntry.getName(subName, sizeof(subName));

      // Skip macOS metadata files (AppleDouble resource forks, .DS_Store)
      if (strncmp(subName, "._", 2) == 0 || strcasecmp(subName, ".DS_Store") == 0) {
        subEntry.close();
        continue;
      }

      const size_t subLen = strlen(subName);
      const bool isIdx = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".idx") == 0;
      const bool isIfo = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".ifo") == 0;
      subEntry.close();

      if (isIfo) ifoCount++;
      if (isIdx) {
        if (foundStem[0] != '\0') {
          // Second .idx found — folder is ambiguous, skip it.
          ambiguous = true;
          LOG_DBG("DSEL", "Skipping %s: multiple .idx files found", name);
          break;
        }
        subName[subLen - 4] = '\0';  // strip ".idx" to get stem
        strncpy(foundStem, subName, sizeof(foundStem) - 1);
      }
    }
    subDir.close();

    if (!ambiguous && ifoCount > 1) {
      ambiguous = true;
      LOG_DBG("DSEL", "Skipping %s: multiple .ifo files found", name);
    }

    if (!ambiguous && foundStem[0] != '\0') {
      dictFolders.push_back(std::string(name));
      dictStems.push_back(std::string(foundStem));
      LOG_DBG("DSEL", "Found dictionary: %s/%s", name, foundStem);
    }
  }

  root.close();

  // Sort alphabetically by folder name (parallel vectors — sort together via paired sort).
  if (dictFolders.size() > 1) {
    // Build index array, sort by folder name, then reorder both vectors.
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(dictFolders.size());
    for (size_t i = 0; i < dictFolders.size(); i++) {
      pairs.push_back({std::move(dictFolders[i]), std::move(dictStems[i])});
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
                // Case-insensitive sort — matches FileBrowserActivity::sortFileList() behaviour.
                const char* s1 = a.first.c_str();
                const char* s2 = b.first.c_str();
                while (*s1 && *s2) {
                  char c1 = static_cast<char>(tolower(static_cast<unsigned char>(*s1)));
                  char c2 = static_cast<char>(tolower(static_cast<unsigned char>(*s2)));
                  if (c1 != c2) return c1 < c2;
                  s1++;
                  s2++;
                }
                return *s1 == '\0' && *s2 != '\0';
              });
    dictFolders.clear();
    dictFolders.reserve(pairs.size());
    dictStems.clear();
    dictStems.reserve(pairs.size());
    for (auto& p : pairs) {
      dictFolders.push_back(std::move(p.first));
      dictStems.push_back(std::move(p.second));
    }
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string DictionarySelectActivity::folderForIndex(int index) const {
  if (index <= 0 || index > static_cast<int>(dictFolders.size())) return "";
  return dictRoot + "/" + dictFolders[index - 1] + "/" + dictStems[index - 1];
}

const char* DictionarySelectActivity::nameForIndex(int index) const {
  if (index == 0) return bookCachePath.empty() ? tr(STR_DICT_NONE) : useGlobalLabel.c_str();
  if (index <= static_cast<int>(dictFolders.size())) return dictFolders[index - 1].c_str();
  return "";
}

void DictionarySelectActivity::applySelection() {
  std::string folder = folderForIndex(selectedIndex);

  if (bookCachePath.empty()) {
    // Settings mode: update global dictionary.bin.
    if (Dictionary::readDictPath() == folder) return;
    Dictionary::saveGlobalDictPath(folder.c_str());
  } else {
    // Per-book mode: save to book cache.
    FsFile f;
    if (Storage.openFileForWrite("DSEL", bookCachePath + "/dictionary.bin", f)) {
      f.write(reinterpret_cast<const uint8_t*>(folder.c_str()), folder.size());
      f.close();
    } else {
      LOG_ERR("DSEL", "Could not save per-book dictionary");
    }
    currentBookDictPath = folder;
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void DictionarySelectActivity::loop() {
  if (showingInfo) {
    if (showingRaw) {
      // Raw view: Back returns to parsed metadata view.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        showingRaw = false;
        requestUpdate();
      }
    } else {
      // Parsed metadata view: Back exits to picker; Confirm switches to raw view.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        showingInfo = false;
        requestUpdate();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        showingRaw = true;
        requestUpdate();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Long press Confirm: show dictionary metadata (only when a real dictionary is highlighted).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= VIEW_INFO_MS &&
      selectedIndex > 0) {
    std::string folder = folderForIndex(selectedIndex);
    currentInfo = Dictionary::readInfo(folder.c_str());
    showingInfo = true;
    showingRaw = false;
    requestUpdate();
    return;
  }

  // Short press Confirm: apply selection (or decompress if compressed) and exit.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < VIEW_INFO_MS) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
      return;
    }

    // For a real dictionary entry, delegate preparation check to DictPrepareActivity.
    // It detects required steps and either runs them or exits immediately if none are needed.
    if (selectedIndex > 0) {
      std::string folder = folderForIndex(selectedIndex);
      startActivityForResult(std::make_unique<DictPrepareActivity>(renderer, mappedInput, folder),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 applySelection();
                                 finish();
                               }
                               // Cancelled/failed: stay in picker with the same highlighted index.
                             });
      return;
    }

    applySelection();
    finish();
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DictionarySelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (showingInfo) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_INFO));

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int x = metrics.contentSidePadding;
    const int maxWidth = pageWidth - metrics.contentSidePadding * 2;
    const int maxY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (showingRaw) {
      // --- Raw view: forward-only SD streaming, character-wrapped per line ---
      char ifoPath[520];
      snprintf(ifoPath, sizeof(ifoPath), "%s.ifo", folderForIndex(selectedIndex).c_str());
      FsFile ifoFile;
      if (!Storage.openFileForRead("DSEL", ifoPath, ifoFile)) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        char segBuf[256];
        int segLen = 0;
        while (y + lineHeight <= maxY) {
          const int firstByte = ifoFile.read();
          if (firstByte == -1) break;
          if (firstByte == '\r') continue;
          if (firstByte == '\n') {
            if (segLen > 0) {
              segBuf[segLen] = '\0';
              renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
              segLen = 0;
            }
            y += lineHeight;
            continue;
          }
          // Determine UTF-8 codepoint length from leading byte.
          int cpLen = 1;
          if ((firstByte & 0xE0) == 0xC0)
            cpLen = 2;
          else if ((firstByte & 0xF0) == 0xE0)
            cpLen = 3;
          else if ((firstByte & 0xF8) == 0xF0)
            cpLen = 4;
          char cpBuf[5];
          cpBuf[0] = static_cast<char>(firstByte);
          for (int i = 1; i < cpLen; i++) {
            const int b = ifoFile.read();
            if (b == -1) {
              cpLen = i;
              break;
            }
            cpBuf[i] = static_cast<char>(b);
          }
          // Flush segment if codepoint won't fit in segBuf.
          if (segLen + cpLen >= static_cast<int>(sizeof(segBuf)) - 1) {
            segBuf[segLen] = '\0';
            renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
            y += lineHeight;
            segLen = 0;
          }
          memcpy(segBuf + segLen, cpBuf, cpLen);
          segLen += cpLen;
          segBuf[segLen] = '\0';
          // If rendered width exceeds column, wrap before this codepoint.
          if (renderer.getTextWidth(UI_10_FONT_ID, segBuf) > maxWidth) {
            segBuf[segLen - cpLen] = '\0';
            if (segLen - cpLen > 0) {
              renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
              y += lineHeight;
            }
            memcpy(segBuf, cpBuf, cpLen);
            segLen = cpLen;
          }
        }
        if (segLen > 0 && y + lineHeight <= maxY) {
          segBuf[segLen] = '\0';
          renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
        }
        ifoFile.close();
      }

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // --- Parsed metadata view ---
      // Short fixed-length fields use single-line truncation; long fields (name, description,
      // website, status) use character-level wrapping so no content is silently cut off (F-001).
      auto drawLine = [&](const char* label, const char* value) {
        if (value == nullptr || value[0] == '\0') return;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", label, value);
        std::string line = renderer.truncatedText(UI_10_FONT_ID, buf, maxWidth);
        renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
        y += lineHeight;
      };

      // Character-level wrapping for values that may exceed one line (URLs, descriptions, etc.).
      auto drawWrapped = [&](const char* label, const char* value) {
        if (value == nullptr || value[0] == '\0') return;
        char buf[384];
        snprintf(buf, sizeof(buf), "%s: %s", label, value);
        const char* text = buf;
        size_t totalLen = strlen(text);
        char segBuf[256];
        size_t pos = 0;
        while (pos < totalLen && y + lineHeight <= maxY) {
          size_t endPos = totalLen;
          while (endPos > pos) {
            size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
            memcpy(segBuf, text + pos, segLen);
            segBuf[segLen] = '\0';
            if (renderer.getTextWidth(UI_10_FONT_ID, segBuf) <= maxWidth) break;
            endPos--;
          }
          if (endPos == pos) endPos = pos + 1;
          size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
          memcpy(segBuf, text + pos, segLen);
          segBuf[segLen] = '\0';
          renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
          y += lineHeight;
          pos = endPos;
        }
      };

      char wordcountBuf[24];
      char synBuf[24];
      snprintf(wordcountBuf, sizeof(wordcountBuf), "%lu", static_cast<unsigned long>(currentInfo.wordcount));
      snprintf(synBuf, sizeof(synBuf), "%lu", static_cast<unsigned long>(currentInfo.altFormCount));

      if (!currentInfo.valid) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        drawWrapped("Name", currentInfo.bookname);
        drawLine("Words", wordcountBuf);
        if (currentInfo.hasAltForms) drawLine("Alt Forms", synBuf);
        drawLine("Date", currentInfo.date);
        drawWrapped("Website", currentInfo.website);
        drawWrapped("Description", currentInfo.description);
        drawLine("Type", currentInfo.sametypesequence);
        if (currentInfo.isCompressed) {
          drawWrapped("Status", "Compressed (.dict.dz) -- extract before use");
        }
      }

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DICT_VIEW_RAW), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

    renderer.displayBuffer();
    return;
  }

  // --- Picker screen ---
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICTIONARY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Show "None found" note when no dictionaries are available
  if (dictFolders.empty()) {
    const int textY = contentTop + contentHeight / 3;
    renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_DICT_NONE_FOUND));
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int index) { return std::string(nameForIndex(index)); }, nullptr, nullptr,
      [this](int index) -> std::string {
        // Show "Selected" marker for the currently active dictionary.
        // In per-book mode compare against currentBookDictPath; in settings mode against global.
        std::string folder = folderForIndex(index);
        const std::string activePath = bookCachePath.empty() ? Dictionary::readDictPath() : currentBookDictPath;
        if (folder.empty() && activePath.empty()) return tr(STR_SELECTED);
        if (!folder.empty() && folder == activePath) return tr(STR_SELECTED);
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
