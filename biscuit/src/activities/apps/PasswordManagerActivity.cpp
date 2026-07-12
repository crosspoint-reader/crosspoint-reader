#include "PasswordManagerActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "PasswordDetailActivity.h"
#include "PasswordGeneratorActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "stores/PasswordStore.h"

int PasswordManagerActivity::getItemCount() const {
  return static_cast<int>(PASSWORD_STORE.size()) + 1;  // +1 for "Generate New"
}

void PasswordManagerActivity::onEnter() {
  Activity::onEnter();
  PASSWORD_STORE.loadFromFile();
  selectorIndex = 0;
  requestUpdate();
}

void PasswordManagerActivity::loop() {
  const int itemCount = getItemCount();

  buttonNavigator.onNext([this, itemCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, itemCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, itemCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& entries = PASSWORD_STORE.getEntries();
    if (selectorIndex < static_cast<int>(entries.size())) {
      startActivityForResult(
          std::make_unique<PasswordDetailActivity>(renderer, mappedInput, selectorIndex),
          [this](const ActivityResult&) {
            // Refresh after returning from detail (entry might have been deleted)
            if (selectorIndex >= getItemCount()) {
              selectorIndex = std::max(0, getItemCount() - 1);
            }
            requestUpdate();
          });
    } else {
      // Generate New
      startActivityForResult(std::make_unique<PasswordGeneratorActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               selectorIndex = std::max(0, getItemCount() - 2);
                               requestUpdate();
                             });
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void PasswordManagerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PASSWORD_MANAGER));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const auto& entries = PASSWORD_STORE.getEntries();
  const int itemCount = getItemCount();

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_NO_PASSWORDS));
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectorIndex,
      [&entries](int index) -> std::string {
        if (index < static_cast<int>(entries.size())) {
          return entries[index].site;
        }
        return std::string("+ ") + tr(STR_GENERATE_PASSWORD);
      },
      [&entries](int index) -> std::string {
        if (index < static_cast<int>(entries.size())) {
          return entries[index].username;
        }
        return "";
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
