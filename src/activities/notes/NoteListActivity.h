#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class NoteListActivity final : public Activity {
 public:
  explicit NoteListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NoteList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator_;
  std::vector<std::string> notes_;
  int selectedIndex_ = 0;
  bool inNameEntry_ = false;
  std::unique_ptr<char[]> nameBuffer_;

  void loadNotes();
  void createNote();
  void openSelected();
  static std::string normalizeNoteName(std::string name);
  static std::string notesPathForName(const std::string& name);
};
