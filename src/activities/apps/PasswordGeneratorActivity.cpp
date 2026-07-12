#include "PasswordGeneratorActivity.h"

#include <I18n.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "stores/PasswordStore.h"

static const char CHARSET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%&*";
static constexpr size_t CHARSET_LEN = sizeof(CHARSET) - 1;

void PasswordGeneratorActivity::generatePassword() {
  generatedPassword.clear();
  generatedPassword.reserve(passwordLength);
  for (int i = 0; i < passwordLength; i++) {
    generatedPassword += CHARSET[esp_random() % CHARSET_LEN];
  }
}

void PasswordGeneratorActivity::onEnter() {
  Activity::onEnter();
  state = GENERATING;
  generatePassword();
  requestUpdate();
}

void PasswordGeneratorActivity::loop() {
  if (state == SAVED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (state == GENERATING) {
    // Up/Down adjust length
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (passwordLength < 32) {
        passwordLength++;
        generatePassword();
        requestUpdate();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (passwordLength > 8) {
        passwordLength--;
        generatePassword();
        requestUpdate();
      }
    }

    // PageForward = regenerate
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      generatePassword();
      requestUpdate();
    }

    // Confirm = proceed to save (enter site name)
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = ENTER_SITE;
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string(tr(STR_SITE)), "", 64),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              state = GENERATING;
              requestUpdate();
              return;
            }
            site = std::get<KeyboardResult>(result.data).text;
            state = ENTER_USERNAME;
            startActivityForResult(
                std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, std::string(tr(STR_USERNAME)), "", 64),
                [this](const ActivityResult& result2) {
                  if (result2.isCancelled) {
                    state = GENERATING;
                    requestUpdate();
                    return;
                  }
                  username = std::get<KeyboardResult>(result2.data).text;
                  PASSWORD_STORE.addEntry(site, username, generatedPassword);
                  state = SAVED;
                  requestUpdate();
                });
          });
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}

void PasswordGeneratorActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GENERATE_PASSWORD));

  if (state == SAVED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_SAVED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Show generated password
  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 40;

  // Length indicator
  std::string lengthStr = std::string(tr(STR_PASSWORD_LENGTH)) + ": " + std::to_string(passwordLength);
  renderer.drawCenteredText(UI_10_FONT_ID, y, lengthStr.c_str());
  y += 50;

  // Password display - may need to wrap for long passwords
  if (generatedPassword.length() <= 20) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, generatedPassword.c_str(), true, EpdFontFamily::BOLD);
  } else {
    // Split into two lines
    std::string line1 = generatedPassword.substr(0, generatedPassword.length() / 2);
    std::string line2 = generatedPassword.substr(generatedPassword.length() / 2);
    renderer.drawCenteredText(UI_10_FONT_ID, y, line1.c_str(), true, EpdFontFamily::BOLD);
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, line2.c_str(), true, EpdFontFamily::BOLD);
  }

  y += 60;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Up/Down: adjust length");
  y += 25;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Side button: regenerate");

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
