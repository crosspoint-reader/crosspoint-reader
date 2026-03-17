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

// SD card root directory for dictionaries.
static constexpr const char* DICT_ROOT = "/dictionary";

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
    const char* activePath = SETTINGS.dictionaryPath;
    if (activePath[0] != '\0') {
      for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
        if (folderForIndex(i + 1) == activePath) {
          selectedIndex = i + 1;
          break;
        }
      }
    }
  } else {
    // Per-book mode: read saved per-book path, pre-select it.
    currentBookDictPath = "";
    FsFile f;
    if (Storage.openFileForRead("DSEL", bookCachePath + "/dictionary.bin", f)) {
      char buf[500];
      int n = f.read(buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        currentBookDictPath = std::string(buf);
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
  dictStems.clear();

  auto root = Storage.open(DICT_ROOT);
  if (!root || !root.isDirectory()) {
    LOG_DBG("DSEL", "No /dictionary directory on SD card");
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  // FAT32 long filenames are max 255 chars; 256 is sufficient for bare names (not full paths).
  char name[256];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));

    if (!entry.isDirectory() || name[0] == '.') {
      entry.close();
      continue;
    }

    // Scan the subdirectory for any .idx file — use the first one found.
    // .ifo is not required; discovery is keyed on .idx presence only.
    char subPath[520];
    snprintf(subPath, sizeof(subPath), "%s/%s", DICT_ROOT, name);
    entry.close();

    auto subDir = Storage.open(subPath);
    if (!subDir || !subDir.isDirectory()) {
      if (subDir) subDir.close();
      continue;
    }

    subDir.rewindDirectory();
    char subName[256];  // bare filename, not full path — 256 is sufficient for FAT32
    char foundStem[256] = "";
    bool multipleIdx = false;
    for (auto subEntry = subDir.openNextFile(); subEntry; subEntry = subDir.openNextFile()) {
      subEntry.getName(subName, sizeof(subName));
      const size_t subLen = strlen(subName);
      const bool isIdx = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".idx") == 0;
      subEntry.close();

      if (isIdx) {
        if (foundStem[0] != '\0') {
          // Second .idx found — folder is ambiguous, skip it.
          multipleIdx = true;
          LOG_DBG("DSEL", "Skipping %s: multiple .idx files found", name);
          break;
        }
        subName[subLen - 4] = '\0';  // strip ".idx" to get stem
        strncpy(foundStem, subName, sizeof(foundStem) - 1);
      }
    }
    subDir.close();

    if (!multipleIdx && foundStem[0] != '\0') {
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
    dictStems.clear();
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
  // Returns the full base path: /dictionary/<folder>/<stem>
  // All file access appends an extension to this (e.g. basePath + ".idx").
  char fullPath[520];
  snprintf(fullPath, sizeof(fullPath), "%s/%s/%s", DICT_ROOT, dictFolders[index - 1].c_str(),
           dictStems[index - 1].c_str());
  return std::string(fullPath);
}

const char* DictionarySelectActivity::nameForIndex(int index) const {
  if (index == 0) return bookCachePath.empty() ? tr(STR_DICT_NONE) : tr(STR_DICT_USE_GLOBAL);
  if (index <= static_cast<int>(dictFolders.size())) return dictFolders[index - 1].c_str();
  return "";
}

void DictionarySelectActivity::applySelection() {
  std::string folder = folderForIndex(selectedIndex);

  if (bookCachePath.empty()) {
    // Settings mode: update global settings.
    strncpy(SETTINGS.dictionaryPath, folder.c_str(), sizeof(SETTINGS.dictionaryPath) - 1);
    SETTINGS.dictionaryPath[sizeof(SETTINGS.dictionaryPath) - 1] = '\0';
    Dictionary::setActivePath(folder.empty() ? "" : folder.c_str());
    SETTINGS.saveToFile();
  } else {
    // Per-book mode: save to book cache, update active path.
    FsFile f;
    if (Storage.openFileForWrite("DSEL", bookCachePath + "/dictionary.bin", f)) {
      f.write(reinterpret_cast<const uint8_t*>(folder.c_str()), folder.size());
      f.close();
    } else {
      LOG_ERR("DSEL", "Could not save per-book dictionary");
    }
    // If "Use Global" selected, restore global active path; otherwise use selected.
    Dictionary::setActivePath(folder.empty() ? SETTINGS.dictionaryPath : folder.c_str());
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
    // Pre-load raw .ifo content for the "View Raw" action within the metadata screen.
    rawIfoContent = "";
    char ifoPath[520];
    snprintf(ifoPath, sizeof(ifoPath), "%s.ifo", folder.c_str());
    FsFile ifoFile;
    if (Storage.openFileForRead("DSEL", ifoPath, ifoFile)) {
      char buf[512];
      int n = ifoFile.read(buf, sizeof(buf) - 1);
      ifoFile.close();
      if (n > 0) {
        buf[n] = '\0';
        rawIfoContent = std::string(buf);
      }
    }
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

    // For a real dictionary entry, check whether any preparation steps are required
    // (extraction of compressed files, generation of .oft index files).
    if (selectedIndex > 0) {
      std::string folder = folderForIndex(selectedIndex);

      // Reuse a single path buffer for all existence checks — 7 x char[520] simultaneously
      // would overflow the loopTask stack (3640 bytes of locals in one frame).
      char pathBuf[520];

      snprintf(pathBuf, sizeof(pathBuf), "%s.dict", folder.c_str());
      const bool dictExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.dict.dz", folder.c_str());
      const bool dictDzExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.idx", folder.c_str());
      const bool idxExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.idx.oft", folder.c_str());
      const bool idxOftExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.syn", folder.c_str());
      const bool synExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.syn.dz", folder.c_str());
      const bool synDzExists = Storage.exists(pathBuf);
      snprintf(pathBuf, sizeof(pathBuf), "%s.syn.oft", folder.c_str());
      const bool synOftExists = Storage.exists(pathBuf);

      const bool needsExtractDict = !dictExists && dictDzExists;
      const bool needsExtractSyn = !synExists && synDzExists;
      const bool needsGenIdx = idxExists && !idxOftExists;
      const bool synWillExist = synExists || synDzExists;
      const bool needsGenSyn = synWillExist && !synOftExists;

      if (needsExtractDict || needsExtractSyn || needsGenIdx || needsGenSyn) {
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
      // --- Raw view: verbatim .ifo file text, character-wrapped per line ---
      if (rawIfoContent.empty()) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        // Single segBuf reused for both measurement and drawing.
        char segBuf[256];
        const char* lineStart = rawIfoContent.c_str();
        while (*lineStart && y + lineHeight <= maxY) {
          const char* lineEnd = lineStart;
          while (*lineEnd && *lineEnd != '\n') lineEnd++;
          size_t lineLen = static_cast<size_t>(lineEnd - lineStart);
          if (lineLen > 0 && lineStart[lineLen - 1] == '\r') lineLen--;

          size_t pos = 0;
          while (pos < lineLen && y + lineHeight <= maxY) {
            size_t endPos = lineLen;
            while (endPos > pos) {
              size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
              memcpy(segBuf, lineStart + pos, segLen);
              segBuf[segLen] = '\0';
              if (renderer.getTextWidth(UI_10_FONT_ID, segBuf) <= maxWidth) break;
              endPos--;
            }
            if (endPos == pos) endPos = pos + 1;
            size_t segLen = endPos - pos < sizeof(segBuf) - 1 ? endPos - pos : sizeof(segBuf) - 1;
            memcpy(segBuf, lineStart + pos, segLen);
            segBuf[segLen] = '\0';
            renderer.drawText(UI_10_FONT_ID, x, y, segBuf);
            y += lineHeight;
            pos = endPos;
          }

          if (*lineEnd == '\0') break;
          lineStart = lineEnd + 1;
        }
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
      snprintf(synBuf, sizeof(synBuf), "%lu", static_cast<unsigned long>(currentInfo.synwordcount));

      if (!currentInfo.valid) {
        renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DICT_NO_METADATA));
      } else {
        drawWrapped("Name", currentInfo.bookname);
        drawLine("Words", wordcountBuf);
        if (currentInfo.hasSyn) drawLine("Synonyms", synBuf);
        drawLine("Date", currentInfo.date);
        drawWrapped("Website", currentInfo.website);
        drawWrapped("Description", currentInfo.description);
        drawLine("Type", currentInfo.sametypesequence);
        if (currentInfo.isCompressed) {
          drawWrapped("Status", "Compressed (.dict.dz) -- extract before use");
        }
      }

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_DICT_VIEW_RAW), "", "");
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
        // In per-book mode compare against currentBookDictPath; in settings mode against SETTINGS.
        std::string folder = folderForIndex(index);
        const std::string& activePath =
            bookCachePath.empty() ? std::string(SETTINGS.dictionaryPath) : currentBookDictPath;
        if (folder.empty() && activePath.empty()) return tr(STR_SELECTED);
        if (!folder.empty() && folder == activePath) return tr(STR_SELECTED);
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
