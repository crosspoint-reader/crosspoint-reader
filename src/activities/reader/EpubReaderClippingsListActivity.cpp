#include "EpubReaderClippingsListActivity.h"

#include <GfxRenderer.h>

#include "ClippingTextViewerActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int EpubReaderClippingsListActivity::getTotalItems() const { return static_cast<int>(clippings.size()); }

void EpubReaderClippingsListActivity::refreshPreviews() {
  previewCache.clear();
  previewCache.reserve(clippings.size());
  for (const auto& entry : clippings) {
    // Read enough to skip markdown headers, but truncate to visible length for fast rendering
    std::string preview = ClippingStore::loadClippingPreview(bookPath, entry, 200);
    if (preview.size() > 55) {
      preview.resize(52);
      preview += "...";
    }
    previewCache.push_back(std::move(preview));
  }
}

int EpubReaderClippingsListActivity::getPageItems() const {
  constexpr int lineHeight = 30;
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = 75 + hintGutterHeight;
  const int availableHeight = screenHeight - startY - lineHeight;
  return std::max(1, availableHeight / lineHeight);
}

void EpubReaderClippingsListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  clippings = ClippingStore::loadIndex(bookPath);
  refreshPreviews();

  if (selectorIndex >= getTotalItems()) {
    selectorIndex = std::max(0, getTotalItems() - 1);
  }

  requestUpdate();
}

void EpubReaderClippingsListActivity::onExit() {
  ActivityWithSubactivity::onExit();
}

void EpubReaderClippingsListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const int totalItems = getTotalItems();

  // Handle empty clippings list
  if (totalItems == 0 && !confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onGoBack();
    }
    return;
  }

  // Delete confirmation mode
  if (confirmingDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ClippingStore::deleteClipping(bookPath, selectorIndex);
      clippings = ClippingStore::loadIndex(bookPath);
      refreshPreviews();
      if (selectorIndex >= getTotalItems()) {
        selectorIndex = std::max(0, getTotalItems() - 1);
      }
      confirmingDelete = false;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      confirmingDelete = false;
      requestUpdate();
    }
    return;
  }

  // Normal navigation
  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() > SKIP_PAGE_MS) {
      confirmingDelete = true;
      requestUpdate();
    } else if (selectorIndex >= 0 && selectorIndex < totalItems) {
      const std::string text = ClippingStore::loadClippingText(bookPath, clippings[selectorIndex]);
      if (!text.empty()) {
        enterNewActivity(new ClippingTextViewerActivity(renderer, mappedInput, text, [this]() {
          exitActivity();
          requestUpdate();
        }));
      }
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    requestUpdate();
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    requestUpdate();
  }
}

void EpubReaderClippingsListActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;
  const int pageItems = getPageItems();
  const int totalItems = getTotalItems();

  const char* titleText = confirmingDelete ? "Delete clipping?" : "Clippings";
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, titleText, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, titleText, true, EpdFontFamily::BOLD);

  if (!confirmingDelete && totalItems > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 40 + contentY, "Hold confirm to delete");
  }

  if (totalItems == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "No clippings", true);
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(contentX, 75 + contentY + (selectorIndex % pageItems) * 30 - 2, contentWidth - 1, 30);

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= totalItems) break;
    const int displayY = 75 + contentY + i * 30;
    const bool isSelected = (itemIndex == selectorIndex);

    const std::string& preview = previewCache[itemIndex];
    const int textX = contentX + 20;
    renderer.drawText(UI_10_FONT_ID, textX, displayY, preview.c_str(), !isSelected);
  }

  if (confirmingDelete) {
    const auto labels = mappedInput.mapLabels("Cancel", "Delete", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels("« Back", "View", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
