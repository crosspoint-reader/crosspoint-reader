#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// SD card root directory for dictionaries.
static constexpr const char* DICT_ROOT = "/dictionary";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();

  scanDictionaries();

  // Validate the currently stored path — resets and saves automatically if no longer valid.
  Dictionary::isValidDictionary();

  // Find which index corresponds to the current setting.
  selectedIndex = 0;  // default: None
  const char* activePath = SETTINGS.dictionaryPath;
  if (activePath[0] != '\0') {
    // activePath is a full path like /dictionary/dict-en-en; extract folder name.
    const char* lastSlash = strrchr(activePath, '/');
    const char* folderName = (lastSlash != nullptr) ? lastSlash + 1 : activePath;
    for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
      if (dictFolders[i] == folderName) {
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

  auto root = Storage.open(DICT_ROOT);
  if (!root || !root.isDirectory()) {
    LOG_DBG("DSEL", "No /dictionary directory on SD card");
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

    // Check for a valid .ifo file inside this subdirectory
    char ifoPath[520];
    snprintf(ifoPath, sizeof(ifoPath), "%s/%s/dict-data.ifo", DICT_ROOT, name);

    if (Storage.exists(ifoPath)) {
      dictFolders.push_back(std::string(name));
      LOG_DBG("DSEL", "Found dictionary: %s", name);
    }

    entry.close();
  }

  root.close();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string DictionarySelectActivity::folderForIndex(int index) const {
  if (index <= 0 || index > static_cast<int>(dictFolders.size())) return "";
  char fullPath[520];
  snprintf(fullPath, sizeof(fullPath), "%s/%s", DICT_ROOT, dictFolders[index - 1].c_str());
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

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    applySelection();
    finish();
    return;
  }

  // Left button: show .ifo info for the highlighted dictionary (not "None")
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && selectedIndex > 0) {
    std::string folder = folderForIndex(selectedIndex);
    currentInfo = Dictionary::readInfo(folder.c_str());
    showingInfo = true;
    requestUpdate();
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
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_DICT_INFO));

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int x = metrics.contentSidePadding;
    const int maxWidth = pageWidth - metrics.contentSidePadding * 2;

    auto drawLine = [&](const char* label, const char* value) {
      if (value == nullptr || value[0] == '\0') return;
      char buf[320];
      snprintf(buf, sizeof(buf), "%s: %s", label, value);
      // Truncate to fit screen width
      std::string line = renderer.truncatedText(UI_10_FONT_ID, buf, maxWidth);
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
    };

    char wordcountBuf[24];
    char synBuf[24];
    snprintf(wordcountBuf, sizeof(wordcountBuf), "%lu", static_cast<unsigned long>(currentInfo.wordcount));
    snprintf(synBuf, sizeof(synBuf), "%lu", static_cast<unsigned long>(currentInfo.synwordcount));

    drawLine("Name", currentInfo.bookname);
    drawLine("Words", wordcountBuf);
    if (currentInfo.hasSyn) drawLine("Synonyms", synBuf);
    drawLine("Date", currentInfo.date);
    drawLine("Website", currentInfo.website);
    drawLine("Description", currentInfo.description);
    drawLine("Type", currentInfo.sametypesequence);
    if (currentInfo.isCompressed) {
      drawLine("Status", "Compressed (.dict.dz) — extract before use");
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // --- Picker screen ---
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_DICTIONARY));

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

  // Show View Info hint as a small note when a real dictionary is highlighted.
  if (selectedIndex > 0) {
    const int hintY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                      renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, hintY, tr(STR_DICT_VIEW_INFO));
  }

  renderer.displayBuffer();
}
