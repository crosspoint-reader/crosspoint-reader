#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * NotesActivity
 *
 * Simple scrollable list of text notes stored in /notes.txt on the SD card.
 * Users can add new notes (via the keyboard) and delete existing ones.
 * Each note is one line in the file.
 */
class NotesActivity final : public Activity {
 public:
  explicit NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notes", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr const char* NOTES_FILE = "/notes.txt";
  static constexpr int MAX_NOTE_LENGTH = 120;
  static constexpr int MAX_NOTES = 100;

  std::vector<std::string> notes;
  int selectedIndex = 0;
  bool dirty = false;

  void loadNotes();
  void saveNotes() const;
  void addNote();
  void deleteSelectedNote();
  ButtonNavigator buttonNavigator{mappedInput};
};
