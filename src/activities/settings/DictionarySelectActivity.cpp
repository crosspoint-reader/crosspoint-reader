#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

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

  ignoreNextConfirmRelease = true;

  scanDictionaries();

  // Validate the currently stored path — resets and saves automatically if no longer valid.
  Dictionary::isValidDictionary();

  // Find which index corresponds to the current setting.
  selectedIndex = 0;  // default: None
  const char* activePath = SETTINGS.dictionaryPath;
  if (activePath[0] != '\0') {
    // activePath is a full base path like /dictionary/dict-en-en/dict-data.
    for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
      if (folderForIndex(i + 1) == activePath) {
        selectedIndex = i + 1;  // +1 because index 0 is "None"
        break;
      }
    }
  }

  totalItems = 1 + static_cast<int>(dictFolders.size());  // None + found dicts
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

    // Scan the subdirectory for any .ifo file — use the first one found.
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
    bool multipleIfo = false;
    for (auto subEntry = subDir.openNextFile(); subEntry; subEntry = subDir.openNextFile()) {
      subEntry.getName(subName, sizeof(subName));
      const size_t subLen = strlen(subName);
      const bool isIfo = !subEntry.isDirectory() && subLen > 4 && strcmp(subName + subLen - 4, ".ifo") == 0;
      subEntry.close();

      if (isIfo) {
        if (foundStem[0] != '\0') {
          // Second .ifo found — folder is invalid.
          multipleIfo = true;
          LOG_DBG("DSEL", "Skipping %s: multiple .ifo files found", name);
          break;
        }
        subName[subLen - 4] = '\0';  // strip ".ifo" to get stem
        strncpy(foundStem, subName, sizeof(foundStem) - 1);
      }
    }
    subDir.close();

    if (!multipleIfo && foundStem[0] != '\0') {
      dictFolders.push_back(std::string(name));
      dictStems.push_back(std::string(foundStem));
      LOG_DBG("DSEL", "Found dictionary: %s/%s", name, foundStem);
    }
  }

  root.close();
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
  if (index == 0) return tr(STR_DICT_NONE);
  if (index <= static_cast<int>(dictFolders.size())) return dictFolders[index - 1].c_str();
  return "";
}

void DictionarySelectActivity::applySelection() {
  std::string folder = folderForIndex(selectedIndex);
  strncpy(SETTINGS.dictionaryPath, folder.c_str(), sizeof(SETTINGS.dictionaryPath) - 1);
  SETTINGS.dictionaryPath[sizeof(SETTINGS.dictionaryPath) - 1] = '\0';
  Dictionary::setActivePath(folder.empty() ? "" : folder.c_str());
  SETTINGS.saveToFile();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void DictionarySelectActivity::loop() {
  if (showingInfo) {
    // Any button dismisses the info screen
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      showingInfo = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Long press Confirm: show dictionary metadata (only when a real dictionary is highlighted).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= VIEW_INFO_MS &&
      selectedIndex > 0) {
    std::string folder = folderForIndex(selectedIndex);
    currentInfo = Dictionary::readInfo(folder.c_str());
    showingInfo = true;
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

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
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
    // --- Info screen: display raw .ifo fields ---
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_INFO));

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int x = metrics.contentSidePadding;
    const int maxWidth = pageWidth - metrics.contentSidePadding * 2;

    auto drawLine = [&](const char* label, const char* value) {
      if (value == nullptr || value[0] == '\0') return;
      char buf[320];
      snprintf(buf, sizeof(buf), "%s: %s", label, value);
      std::string line = renderer.truncatedText(UI_10_FONT_ID, buf, maxWidth);
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
    };

    // Character-level line breaking for values with no spaces (e.g. URLs).
    // Matches the pattern used in KeyboardEntryActivity for long input text.
    // Truncates only the final line if the cap is reached.
    auto drawCharWrapped = [&](const char* label, const char* value, int maxLines) {
      if (value == nullptr || value[0] == '\0') return;
      char buf[320];
      snprintf(buf, sizeof(buf), "%s: %s", label, value);
      const std::string text(buf);
      int startIdx = 0;
      int linesDrawn = 0;
      while (startIdx < static_cast<int>(text.size()) && linesDrawn < maxLines) {
        if (linesDrawn == maxLines - 1) {
          // Last allowed line: truncate remainder with ellipsis.
          std::string remaining = text.substr(startIdx);
          std::string truncated = renderer.truncatedText(UI_10_FONT_ID, remaining.c_str(), maxWidth);
          renderer.drawText(UI_10_FONT_ID, x, y, truncated.c_str());
          y += lineHeight;
          break;
        }
        // Find the longest prefix of the remaining text that fits on one line.
        int endIdx = static_cast<int>(text.size());
        while (endIdx > startIdx) {
          std::string segment = text.substr(startIdx, endIdx - startIdx);
          if (renderer.getTextWidth(UI_10_FONT_ID, segment.c_str()) <= maxWidth) break;
          endIdx--;
        }
        renderer.drawText(UI_10_FONT_ID, x, y, text.substr(startIdx, endIdx - startIdx).c_str());
        y += lineHeight;
        linesDrawn++;
        startIdx = endIdx;
      }
    };

    char wordcountBuf[24];
    char synBuf[24];
    snprintf(wordcountBuf, sizeof(wordcountBuf), "%lu", static_cast<unsigned long>(currentInfo.wordcount));
    snprintf(synBuf, sizeof(synBuf), "%lu", static_cast<unsigned long>(currentInfo.synwordcount));

    drawLine("Name", currentInfo.bookname);
    drawLine("Words", wordcountBuf);
    if (currentInfo.hasSyn) drawLine("Synonyms", synBuf);
    drawLine("Date", currentInfo.date);
    drawCharWrapped("Website", currentInfo.website, 5);
    drawLine("Description", currentInfo.description);
    drawLine("Type", currentInfo.sametypesequence);
    if (currentInfo.isCompressed) {
      drawLine("Status", "Compressed (.dict.dz) — extract before use");
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
        // Show "Selected" marker for current active dictionary
        std::string folder = folderForIndex(index);
        const char* activePath = SETTINGS.dictionaryPath;
        if (folder.empty() && activePath[0] == '\0') return tr(STR_SELECTED);
        if (!folder.empty() && folder == activePath) return tr(STR_SELECTED);
        return "";
      },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
