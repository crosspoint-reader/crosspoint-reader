#pragma once

#include <string>

#include "../MenuListActivity.h"

// Context menu shown when the user long-presses the info button on a file in the file browser.
// The available actions depend on file type.  The selected action is returned via MenuResult::action.
class FileContextMenuActivity final : public MenuListActivity {
 public:
  enum class Action {
    None = -1,
    Open = 0,
    FetchAndOpen,
    MarkAsRead,
    Info,
    DeleteCache,
    SetAsSleepCover,
    FlashFirmware,
    Remove,
  };

  explicit FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& filePath);

  void render(RenderLock&&) override;

 private:
  std::string filePath;

  void onActionSelected(int index) override;
  void onBackPressed() override;
};
