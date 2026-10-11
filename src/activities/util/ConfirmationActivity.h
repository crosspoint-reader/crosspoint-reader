#pragma once
#include <I18n.h>

#include <optional>
#include <string>

#include "../Activity.h"
#include "components/UiAppHost.h"

// The one way a screen asks a two-answer question, or one with a third answer between Cancel and
// accept. Built on fui::optionDialog rather than hand-drawn text, so the answers are on-screen
// buttons a finger can hit.
//
// The hint strip is still drawn, because it is how the physical buttons are labelled and it is the
// only affordance a non-touch board has. The two are alternatives, not duplicates.
//
// A screen whose question is one step of its own work (Check for Updates, Clear Cache) pushes this
// too, and does the work from the result: the answer keys, the hints and the touch targets are then
// the same on every question, instead of each screen wiring its own.
class ConfirmationActivity : public Activity, private UiAppHost {
 public:
  // What the prompt says, and what its answers are called. A headline or a message is enough; the
  // labels default to Cancel / Confirm. An answer that starts something names it ("Update",
  // "Clear"), and a question that is not about doing something is answered No / Yes. The line caps
  // are ConfirmDialog::Spec's: the message gives lines back if the panel would not fit the screen.
  struct Question {
    std::string title;  // a small caption above the headline
    std::string headline;
    std::string message;
    StrId cancelLabel = StrId::STR_CANCEL;
    StrId acceptLabel = StrId::STR_CONFIRM;
    // A third answer, drawn between the other two and reported as ConfirmResult::alternate. With
    // one, Back is labelled as Cancel and the two front-strip keys answer alternate and accept.
    std::optional<StrId> alternateLabel;
    uint8_t titleMaxLines = 1;
    uint8_t headlineMaxLines = 3;
    uint8_t messageMaxLines = 4;
  };

  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Question question);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  Question question;
  bool inputArmed = false;

  static void dialogScreen(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onConfirmEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onAlternateEvent(const freeink::ui::ActionEvent& event, void* user);

  enum class Answer : uint8_t { Cancel, Accept, Alternate };

  void buildDialogScreen(UiScreen& screen);
  void finishWith(Answer answer);
};
