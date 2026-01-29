#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "Xtc.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
int page = 1;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  return 25; // 固定返回25
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  renderer.clearScreen();
  Activity::onEnter();

  updateRequired = true;
  selectorIndex = 0;
  page = 1;
  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionTask",
              4096,        
              this,        
              1,           
              &displayTaskHandle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
}

void XtcReaderChapterSelectionActivity::loop() {
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int pagebegin=(page-1)*25;
    xtc->readChapters_gd(pagebegin);
    uint32_t chapterpage = this->xtc->getChapterstartpage(selectorIndex);
    Serial.printf("[%lu] [XTC] 跳转章节：%d,跳转页数：%d\n", millis(), selectorIndex, chapterpage);
    
    onSelectPage(chapterpage);
    // 确认按键逻辑，按需补充
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    bool isUpKey = mappedInput.wasReleased(MappedInputManager::Button::Up);
    if (skipPage || isUpKey) {
      page -= 1;
      if(page < 1) page = 1; 
      selectorIndex = (page-1)*25; 
    } else {
      selectorIndex--;
      if(selectorIndex < 0) selectorIndex = 0; 
    }
    updateRequired = true;
  } else if (nextReleased) {
    bool isDownKey = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (skipPage || isDownKey) {
      page += 1;
      selectorIndex = (page-1)*25; 
    } else {
      selectorIndex++; 
    }
    updateRequired = true;
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      renderScreen();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();
  const int pagebegin=(page-1)*25;
  int page_chapter=25;
  static int parsedPage = -1;

  if (parsedPage != page) {
    xtc->readChapters_gd(pagebegin);
    parsedPage = page;
  }

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Chapter", true, EpdFontFamily::BOLD);

  const int FIX_LINE_HEIGHT = 29;
  const int BASE_Y = 60;


  for (int i = pagebegin; i <= pagebegin + page_chapter - 1; i++) {
      int localIdx = i - pagebegin; 
      
      uint32_t currOffset = this->xtc->getChapterstartpage(i); 
      std::string dirTitle = this->xtc->getChapterTitleByIndex(i); 
      
      Serial.printf("[%lu] [XTC_CHAPTER] 第%d章，名字为:%s,页码为%d\n", millis(), i, dirTitle.c_str(),currOffset);
      static char title[64];
      strncpy(title, dirTitle.c_str(), sizeof(title)-1);
      title[sizeof(title)-1] = '\0';
      
      int drawY = BASE_Y + localIdx * FIX_LINE_HEIGHT; 

      Serial.printf("选中的选项是：%d\n",selectorIndex);
      renderer.drawText(UI_10_FONT_ID, 20, drawY, title, i!= selectorIndex); 
  }

  renderer.displayBuffer();
}
