#pragma once
#include <memory>
#include <string>

#include "Dictionary.h"
#include "LookupHistory.h"
#include "WordSelectNavigator.h"

class Activity;
class DictLookupTask;
class GfxRenderer;
class MappedInputManager;

// Shared controller for dictionary lookup flow used by DictionaryWordSelectActivity
// and DictionaryDefinitionActivity.  Owns the background lookup task, the
// stems/alt-form fallback chain, and all overlay rendering (looking-up popup,
// alt-form prompt, not-found popup).  The calling activity delegates input and
// render to this class whenever isActive() returns true.
class DictionaryLookupController {
  friend class DictLookupTask;

 public:
  enum class LookupState { Idle, LookingUp, AltFormPrompt, NotFound };
  enum class LookupEvent { None, FoundDefinition, NotFoundDismissedBack, NotFoundDismissedDone, Cancelled };

  // How the word was ultimately resolved when FoundDefinition fires.
  enum class FoundStatus { Direct, Stem, AltForm, Suggestion };

  // Convert FoundStatus to LookupHistory::Status for history recording.
  static LookupHistory::Status toHistStatus(FoundStatus fs) {
    switch (fs) {
      case FoundStatus::Direct:
        return LookupHistory::Status::Direct;
      case FoundStatus::Stem:
        return LookupHistory::Status::Stem;
      case FoundStatus::AltForm:
        return LookupHistory::Status::AltForm;
      case FoundStatus::Suggestion:
        return LookupHistory::Status::Suggestion;
      default:
        return LookupHistory::Status::NotFound;
    }
  }

  DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput, Activity& owner,
                             std::string cachePath = "");
  ~DictionaryLookupController();

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

  // Show the "no word" popup with a 1-second delay, then request update.
  void showNoWordPopup();

  // Clean the word and start lookup; shows no-word popup if cleaning yields empty.
  void lookupOrPopup(const std::string& rawWord);

  // Handle multi-select input from the navigator. Returns true if input was consumed
  // (caller should return from loop). Cleans the phrase and starts lookup or shows popup.
  bool handleMultiSelect(WordSelectNavigator& navigator);

  // Handle single-word confirm lookup from the navigator. Returns true if input was consumed
  // (caller should return from loop). Gets the selected word and starts lookup or shows popup.
  bool handleConfirmLookup(const WordSelectNavigator& navigator);

  const std::string& getLookupWord() const { return lookupWord; }
  const std::string& getFoundWord() const { return foundWord; }
  const DictLocation& getFoundLocation() const { return foundLocation; }
  FoundStatus getFoundStatus() const { return foundStatus; }

 private:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity& owner;
  std::string cachePath;

  LookupState state = LookupState::Idle;
  FoundStatus foundStatus = FoundStatus::Direct;
  bool nextIsSuggestion = false;

  std::string lookupWord;
  std::string foundWord;
  DictLocation foundLocation;
  std::string altFormWord;

  volatile int lookupProgress = 0;
  volatile bool lookupDone = false;
  volatile bool lookupCancelled = false;
  volatile bool lookupCancelRequested = false;

  std::unique_ptr<DictLookupTask> task;

  void runLookup();
  void handleLookupFailed();
  static void progressCallback(void* ctx, int percent);
  static bool cancelCallback(void* ctx);
};
