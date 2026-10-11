#include "ConfirmationActivity.h"

#include <I18n.h>

#include <utility>

#include "../../components/ConfirmDialog.h"
#include "../../components/UITheme.h"
#include "HalDisplay.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CANCEL = 1;
constexpr fui::ActionId ACTION_CONFIRM = 2;
constexpr fui::ActionId ACTION_ALTERNATE = 3;
}  // namespace

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : ConfirmationActivity(renderer, mappedInput, Question{"", heading, body}) {}

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Question question)
    : Activity("Confirmation", renderer, mappedInput), UiAppHost(renderer), question(std::move(question)) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  inputArmed = false;

  resetUi();
  app.on(ACTION_CANCEL, &ConfirmationActivity::onCancelEvent, this);
  app.on(ACTION_CONFIRM, &ConfirmationActivity::onConfirmEvent, this);
  app.on(ACTION_ALTERNATE, &ConfirmationActivity::onAlternateEvent, this);
  app.setScreen(&ConfirmationActivity::dialogScreen, this);
  requestUpdate(true);
}

void ConfirmationActivity::onExit() {
  closeRouting();
  Activity::onExit();
}

void ConfirmationActivity::finishWith(const Answer answer) {
  ActivityResult res;
  res.isCancelled = answer == Answer::Cancel;
  if (!res.isCancelled) res.data = ConfirmResult{answer == Answer::Alternate};
  setResult(std::move(res));
  finish();
}

void ConfirmationActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  static_cast<ConfirmationActivity*>(user)->finishWith(Answer::Cancel);
}

void ConfirmationActivity::onConfirmEvent(const fui::ActionEvent&, void* user) {
  static_cast<ConfirmationActivity*>(user)->finishWith(Answer::Accept);
}

void ConfirmationActivity::onAlternateEvent(const fui::ActionEvent&, void* user) {
  static_cast<ConfirmationActivity*>(user)->finishWith(Answer::Alternate);
}

void ConfirmationActivity::dialogScreen(UiScreen& screen, void* user) {
  static_cast<ConfirmationActivity*>(user)->buildDialogScreen(screen);
}

void ConfirmationActivity::buildDialogScreen(UiScreen& screen) {
  ConfirmDialog::Spec spec;
  spec.title = question.title.empty() ? nullptr : question.title.c_str();
  spec.headline = question.headline.empty() ? nullptr : question.headline.c_str();
  spec.message = question.message.empty() ? nullptr : question.message.c_str();
  spec.cancelLabel = I18N.get(question.cancelLabel);
  spec.acceptLabel = I18N.get(question.acceptLabel);
  spec.alternateLabel = question.alternateLabel ? I18N.get(*question.alternateLabel) : nullptr;
  spec.cancelAction = ACTION_CANCEL;
  spec.acceptAction = ACTION_CONFIRM;
  spec.alternateAction = ACTION_ALTERNATE;
  spec.titleMaxLines = question.titleMaxLines;
  spec.headlineMaxLines = question.headlineMaxLines;
  spec.messageMaxLines = question.messageMaxLines;
  ConfirmDialog::draw(screen, spec);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  renderUi();

  // Still drawn, and not redundant: this is how the PHYSICAL buttons are labelled, and on a board
  // with no digitiser it is the only affordance there is. With a third answer the front pair answers
  // alternate and accept, and Back, which cancels either way, carries the Cancel label.
  const char* cancelLabel = I18N.get(question.cancelLabel);
  const char* acceptLabel = I18N.get(question.acceptLabel);
  const auto labels = question.alternateLabel
                          ? mappedInput.mapLabels(cancelLabel, "", I18N.get(*question.alternateLabel), acceptLabel)
                          : mappedInput.mapLabels("", "", cancelLabel, acceptLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (!inputArmed) {
    const bool anyFrontPressed = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                                 mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                                 mappedInput.isPressed(MappedInputManager::Button::Left) ||
                                 mappedInput.isPressed(MappedInputManager::Button::Right);

    // Ignore inherited press/release events from the parent activity.
    if (!anyFrontPressed && !mappedInput.wasAnyPressed() && !mappedInput.wasAnyReleased()) {
      inputArmed = true;
    }
    return;
  }

  // Touch first: a tap that lands on a dialog button is answered by that button, and must not also
  // reach the button-release tests below.
  const auto touch = routeTouch(mappedInput);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) return;  // a handler ran; it has already called finish()
  }

  // Back leaves every screen, so it leaves this one too, as Cancel. Tested before the answer keys: if
  // a remap ever put Back on the accepting key, the press must not be taken as a yes.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishWith(Answer::Cancel);
    return;
  }

  // The answers are not travel across a screen, so they do not follow the logical directions onto
  // the side buttons: they stay on the front strip, which is the only strip this prompt draws hints
  // on. What does follow the orientation is which of the two is drawn first — and these are the
  // very buttons mapLabels() puts those two labels on (Cancel or the alternate, then accept).
  if (mappedInput.wasReleased(MappedInputManager::frontStripNext())) {
    finishWith(Answer::Accept);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::frontStripPrevious())) {
    finishWith(question.alternateLabel ? Answer::Alternate : Answer::Cancel);
    return;
  }
}
