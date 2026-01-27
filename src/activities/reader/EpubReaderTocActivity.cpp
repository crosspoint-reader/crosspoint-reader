#include "EpubReaderTocActivity.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"

namespace {
constexpr int TAB_BAR_Y = 15;
constexpr int CONTENT_START_Y = 60;
constexpr int CHAPTER_LINE_HEIGHT = 30;
constexpr int FOOTNOTE_LINE_HEIGHT = 40;
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

void EpubReaderTocActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderTocActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderTocActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();

  // Init chapters state
  buildFilteredChapterList();
  chaptersSelectorIndex = 0;
  for (size_t i = 0; i < filteredSpineIndices.size(); i++) {
    if (filteredSpineIndices[i] == currentSpineIndex) {
      chaptersSelectorIndex = i;
      break;
    }
  }
  if (hasSyncOption()) {
    chaptersSelectorIndex += 1;
  }

  // Init footnotes state
  footnotesSelectedIndex = 0;

  updateRequired = true;
  xTaskCreate(&EpubReaderTocActivity::taskTrampoline, "EpubReaderTocTask", 4096, this, 1, &displayTaskHandle);
}

void EpubReaderTocActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void EpubReaderTocActivity::launchSyncActivity() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new KOReaderSyncActivity(
      renderer, mappedInput, this->epub, epubPath, currentSpineIndex, currentPage, totalPagesInSpine,
      [this]() {
        exitActivity();
        this->updateRequired = true;
      },
      [this](int newSpineIndex, int newPage) {
        exitActivity();
        this->onSyncPosition(newSpineIndex, newPage);
      }));
  xSemaphoreGive(renderingMutex);
}

void EpubReaderTocActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }

  const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (leftReleased && currentTab == Tab::FOOTNOTES) {
    currentTab = Tab::CHAPTERS;
    updateRequired = true;
    return;
  }
  if (rightReleased && currentTab == Tab::CHAPTERS) {
    currentTab = Tab::FOOTNOTES;
    updateRequired = true;
    return;
  }

  if (currentTab == Tab::CHAPTERS) {
    loopChapters();
  } else {
    loopFootnotes();
  }
}

void EpubReaderTocActivity::loopChapters() {
  const bool upReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
  const bool downReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int totalItems = getChaptersTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isSyncItem(chaptersSelectorIndex)) {
      launchSyncActivity();
      return;
    }

    int filteredIndex = chaptersSelectorIndex;

    if (hasSyncOption() && chaptersSelectorIndex > 0) filteredIndex -= 1;

    if (filteredIndex >= 0 && filteredIndex < static_cast<int>(filteredSpineIndices.size())) {
      onSelectSpineIndex(filteredSpineIndices[filteredIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (upReleased) {
    if (totalItems > 0) {
      if (skipPage) {
        // TODO: implement page-skip navigation once page size is available
      }
      chaptersSelectorIndex = (chaptersSelectorIndex + totalItems - 1) % totalItems;
      updateRequired = true;
    }
  } else if (downReleased) {
    if (totalItems > 0) {
      if (skipPage) {
        // TODO: implement page-skip navigation once page size is available
      }
      chaptersSelectorIndex = (chaptersSelectorIndex + 1) % totalItems;
      updateRequired = true;
    }
  }
}

void EpubReaderTocActivity::loopFootnotes() {
  bool needsRedraw = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (footnotesSelectedIndex > 0) {
      footnotesSelectedIndex--;
      needsRedraw = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (footnotesSelectedIndex < footnotes.getCount() - 1) {
      footnotesSelectedIndex++;
      needsRedraw = true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const FootnoteEntry* entry = footnotes.getEntry(footnotesSelectedIndex);
    if (entry) {
      onSelectFootnote(entry->href);
    }
  }
  if (needsRedraw) {
    updateRequired = true;
  }
}

void EpubReaderTocActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void EpubReaderTocActivity::renderScreen() {
  renderer.clearScreen();

  std::vector<TabInfo> tabs = {{"Chapters", currentTab == Tab::CHAPTERS}, {"Footnotes", currentTab == Tab::FOOTNOTES}};
  ScreenComponents::drawTabBar(renderer, TAB_BAR_Y, tabs);

  const int screenHeight = renderer.getScreenHeight();
  const int contentHeight = screenHeight - CONTENT_START_Y - 60;

  if (currentTab == Tab::CHAPTERS) {
    renderChapters(CONTENT_START_Y, contentHeight);
  } else {
    renderFootnotes(CONTENT_START_Y, contentHeight);
  }

  ScreenComponents::drawScrollIndicator(renderer, getCurrentPage(), getTotalPages(), CONTENT_START_Y, contentHeight);

  const auto labels = mappedInput.mapLabels("« Back", "Select", "< Tab", "Tab >");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.drawSideButtonHints(UI_10_FONT_ID, ">", "<");

  renderer.displayBuffer();
}

void EpubReaderTocActivity::renderChapters(int contentTop, int contentHeight) {
  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getChaptersPageItems(contentHeight);
  const int totalItems = getChaptersTotalItems();
  const auto pageStartIndex = chaptersSelectorIndex / pageItems * pageItems;

  renderer.fillRect(0, contentTop + (chaptersSelectorIndex % pageItems) * CHAPTER_LINE_HEIGHT - 2, pageWidth - 1,
                    CHAPTER_LINE_HEIGHT);

  for (int i = 0; i < pageItems; i++) {
    int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;

    const int displayY = contentTop + i * CHAPTER_LINE_HEIGHT;
    const bool isSelected = (itemIndex == chaptersSelectorIndex);

    if (isSyncItem(itemIndex)) {
      renderer.drawText(UI_10_FONT_ID, 20, displayY, ">> Sync Progress", !isSelected);
    } else {
      int filteredIndex = itemIndex;
      if (hasSyncOption()) filteredIndex -= 1;

      if (filteredIndex >= 0 && filteredIndex < static_cast<int>(filteredSpineIndices.size())) {
        int spineIndex = filteredSpineIndices[filteredIndex];
        int tocIndex = this->epub->getTocIndexForSpineIndex(spineIndex);
        if (tocIndex == -1) {
          renderer.drawText(UI_10_FONT_ID, 20, displayY, "Unnamed", !isSelected);
        } else {
          auto item = this->epub->getTocItem(tocIndex);
          const int indentSize = 20 + (item.level - 1) * 15;
          const std::string chapterName =
              renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), pageWidth - 40 - indentSize);
          renderer.drawText(UI_10_FONT_ID, indentSize, displayY, chapterName.c_str(), !isSelected);
        }
      }
    }
  }
}

void EpubReaderTocActivity::renderFootnotes(int contentTop, int contentHeight) {
  const int marginLeft = 20;
  if (footnotes.getCount() == 0) {
    renderer.drawText(SMALL_FONT_ID, marginLeft, contentTop + 20, "No footnotes on this page");
    return;
  }
  for (int i = 0; i < footnotes.getCount(); i++) {
    const FootnoteEntry* entry = footnotes.getEntry(i);
    if (!entry) continue;
    const int y = contentTop + i * FOOTNOTE_LINE_HEIGHT;
    if (i == footnotesSelectedIndex) {
      renderer.drawText(UI_12_FONT_ID, marginLeft - 10, y, ">", EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, marginLeft + 10, y, entry->number, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_12_FONT_ID, marginLeft + 10, y, entry->number);
    }
  }
}

void EpubReaderTocActivity::buildFilteredChapterList() {
  filteredSpineIndices.clear();
  for (int i = 0; i < this->epub->getSpineItemsCount(); i++) {
    if (this->epub->shouldHideFromToc(i)) continue;
    int tocIndex = this->epub->getTocIndexForSpineIndex(i);
    if (tocIndex == -1) continue;
    filteredSpineIndices.push_back(i);
  }
}

bool EpubReaderTocActivity::hasSyncOption() const { return KOREADER_STORE.hasCredentials(); }

bool EpubReaderTocActivity::isSyncItem(int index) const {
  if (!hasSyncOption()) return false;
  return index == 0 || index == getChaptersTotalItems() - 1;
}

int EpubReaderTocActivity::getChaptersTotalItems() const {
  const int syncCount = hasSyncOption() ? 2 : 0;
  return filteredSpineIndices.size() + syncCount;
}

int EpubReaderTocActivity::getChaptersPageItems(int contentHeight) const {
  int items = contentHeight / CHAPTER_LINE_HEIGHT;
  return (items < 1) ? 1 : items;
}

int EpubReaderTocActivity::getCurrentPage() const {
  if (currentTab == Tab::CHAPTERS) {
    const int availableHeight = renderer.getScreenHeight() - 120;
    const int itemsPerPage = availableHeight / CHAPTER_LINE_HEIGHT;
    return chaptersSelectorIndex / (itemsPerPage > 0 ? itemsPerPage : 1) + 1;
  }
  return 1;
}

int EpubReaderTocActivity::getTotalPages() const {
  if (currentTab == Tab::CHAPTERS) {
    const int availableHeight = renderer.getScreenHeight() - 120;
    const int itemsPerPage = availableHeight / CHAPTER_LINE_HEIGHT;
    const int totalItems = getChaptersTotalItems();
    if (totalItems == 0) return 1;
    return (totalItems + itemsPerPage - 1) / (itemsPerPage > 0 ? itemsPerPage : 1);
  }
  return 1;
}
