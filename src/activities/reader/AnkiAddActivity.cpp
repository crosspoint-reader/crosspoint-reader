#include "AnkiAddActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <ctime>

#include "ScreenComponents.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/AnkiStore.h"

AnkiAddActivity::AnkiAddActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string frontText,
                                 std::string contextText)
    : Activity("AnkiAdd", renderer, mappedInput),
      frontText(std::move(frontText)),
      contextText(std::move(contextText)) {}

void AnkiAddActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void AnkiAddActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    util::AnkiCard card;
    card.front = frontText;
    card.back = "";  // Needs back-side input logic
    card.context = contextText;

    const std::time_t now = std::time(nullptr);
    if (now > 1577836800) {  // 2020-01-01
      card.timestamp = static_cast<uint32_t>(now);
    } else {
      card.timestamp = static_cast<uint32_t>(millis() / 1000);
    }

    util::AnkiStore::getInstance().addCard(card);
    util::AnkiStore::getInstance().save();
    saved = true;
    requestUpdate();
    vTaskDelay(pdMS_TO_TICKS(500));  // Show "Saved" briefly
    finish();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void AnkiAddActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 40, tr(STR_ADD_TO_ANKI), true, EpdFontFamily::BOLD);

  // Card Preview box
  const int boxMargin = 30;
  const int boxWidth = pageWidth - (boxMargin * 2);
  const int boxHeight = 150;
  const int boxY = 80;

  renderer.drawRect(boxMargin, boxY, boxWidth, boxHeight);

  // Truncated front text
  std::string displayFront = renderer.truncatedText(UI_10_FONT_ID, frontText.c_str(), boxWidth - 20);
  renderer.drawCenteredText(UI_10_FONT_ID, boxY + 40, displayFront.c_str());

  renderer.drawCenteredText(UI_10_FONT_ID, boxY + 80, "...", false, EpdFontFamily::ITALIC);

  if (saved) {
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + boxHeight + 40, tr(STR_ANKI_SAVED), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, boxY + boxHeight + 40, tr(STR_ANKI_SAVE_PROMPT));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SAVE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
