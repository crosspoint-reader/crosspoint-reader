#include "PasswordDetailActivity.h"

#include <I18n.h>

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "stores/PasswordStore.h"

void PasswordDetailActivity::onEnter() {
  Activity::onEnter();
  state = VIEWING;
  showPassword = false;
  requestUpdate();
}

void PasswordDetailActivity::loop() {
  if (state == DELETED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (state == CONFIRM_DELETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      PASSWORD_STORE.removeEntry(entryIndex);
      state = DELETED;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = VIEWING;
      requestUpdate();
    }
    return;
  }

  // VIEWING: short press Confirm = show/hide, long press Confirm (500ms+) = delete
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= 500) {
      state = CONFIRM_DELETE;
    } else {
      showPassword = !showPassword;
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void PasswordDetailActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (state == DELETED) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PASSWORD_MANAGER));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_ENTRY_DELETED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CONFIRM_DELETE) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PASSWORD_MANAGER));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_DELETE_ENTRY), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // VIEWING
  const auto& entries = PASSWORD_STORE.getEntries();
  if (entryIndex >= static_cast<int>(entries.size())) {
    finish();
    return;
  }
  const auto& entry = entries[entryIndex];

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, entry.site.c_str());

  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 20;
  const int lineHeight = 50;

  // Site
  renderer.drawText(SMALL_FONT_ID, leftPad, y, tr(STR_SITE), EpdFontFamily::BOLD);
  y += 25;
  renderer.drawText(UI_10_FONT_ID, leftPad, y, entry.site.c_str());
  y += lineHeight;

  // Username
  renderer.drawText(SMALL_FONT_ID, leftPad, y, tr(STR_USERNAME), EpdFontFamily::BOLD);
  y += 25;
  renderer.drawText(UI_10_FONT_ID, leftPad, y, entry.username.c_str());
  y += lineHeight;

  // Password
  renderer.drawText(SMALL_FONT_ID, leftPad, y, tr(STR_PASSWORD), EpdFontFamily::BOLD);
  y += 25;
  if (showPassword) {
    renderer.drawText(UI_10_FONT_ID, leftPad, y, entry.password.c_str());
  } else {
    std::string masked(entry.password.length(), '*');
    renderer.drawText(UI_10_FONT_ID, leftPad, y, masked.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showPassword ? tr(STR_HIDE) : tr(STR_SHOW), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, "Hold: Delete", labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
