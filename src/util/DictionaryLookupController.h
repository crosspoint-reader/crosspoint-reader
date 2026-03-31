#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

// Shared controller for dictionary lookup flow used by DictionaryWordSelectActivity
// and DictionaryDefinitionActivity.  Owns the background lookup task, the
// stems/alt-form fallback chain, and all overlay rendering (looking-up popup,
// alt-form prompt, not-found popup).  The calling activity delegates input and
// render to this class whenever isActive() returns true.
class DictionaryLookupController {
 public:
  enum class LookupState { Idle, LookingUp, AltFormPrompt, NotFound };
  enum class LookupEvent {
    None,
    FoundDefinition,
    LookupFailed,
    NotFoundDismissedBack,
    NotFoundDismissedDone,
    Cancelled
  };

  // How the word was ultimately resolved when FoundDefinition fires.
  enum class FoundStatus { Direct, Stem, AltForm, Suggestion };

  DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput, Activity& owner);

  // Start a lookup.  Transitions Idle → LookingUp, spawns background task.
  void startLookup(const std::string& word);

  // Like startLookup but marks the result as Suggestion (word came from fuzzy suggestions list).
  void startLookupAsSuggestion(const std::string& word);

  // Called by the activity after the suggestions path has been exhausted.
  // Transitions to NotFound state.
  void setNotFound();

  // Must be called from the activity's onExit() to kill any running task.
  void onExit();

  // True when the controller owns input/render (LookingUp, AltFormPrompt, NotFound).
  bool isActive() const { return state != LookupState::Idle; }

  // Process input for the current state.  Returns an event the activity must handle.
  LookupEvent handleInput();

  // Draw the appropriate overlay and call displayBuffer.  Returns true when fully
  // handled (activity must return immediately from render()).
  bool render();

  // Inform the activity's skipLoopDelay() override.
  bool skipLoopDelay() const { return state == LookupState::LookingUp; }

  const std::string& getLookupWord() const { return lookupWord; }
  const std::string& getFoundWord() const { return foundWord; }
  const std::string& getFoundDefinition() const { return foundDefinition; }
  FoundStatus getFoundStatus() const { return foundStatus; }

 private:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity& owner;

  LookupState state = LookupState::Idle;
  FoundStatus foundStatus = FoundStatus::Direct;
  bool nextIsSuggestion = false;

  std::string lookupWord;
  std::string foundWord;
  std::string foundDefinition;
  std::string altFormWord;

  volatile int lookupProgress = 0;
  volatile bool lookupDone = false;
  volatile bool lookupCancelled = false;
  volatile bool lookupCancelRequested = false;

  TaskHandle_t taskHandle = nullptr;

  void runLookup();
  static void taskEntry(void* param);
  static void progressCallback(void* ctx, int percent);
  static bool cancelCallback(void* ctx);
};
