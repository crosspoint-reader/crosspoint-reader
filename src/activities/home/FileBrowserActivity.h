#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // File actions
  bool removeDirFile(const std::string& fullPath);
  void showEntryActions();
  void startRename();
  void renameSelectedFile(const std::string& oldPath, const std::string& oldEntry, const std::string& newStem,
                          const std::string& extension);
  void deleteSelected();

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;
  OptionPopup optionPopup;

  // Per-row render buffers, derived from `files` and rebuilt only when it
  // changes (loadFiles()) rather than on every repaint — buildScreen() used to
  // rebuild a name/extension string and a ListItem per file on every render
  // (cursor move, tap flash, ...), which meant a 500-file directory allocated
  // 500 strings per repaint instead of once per directory load.
  std::vector<std::string> rowNames;
  std::vector<std::string> rowExtensions;
  std::vector<freeink::ui::ListItem> rowItems;
  // getFileName()'s "[folder]" bracket formatting depends on the active
  // theme's showsFileIcons(); tracked so a theme change while this activity is
  // paused underneath (e.g. a Settings screen reached via a picker flow)
  // invalidates the cached rows on return instead of rendering stale ones.
  bool rowsUseFileIcons = false;

  void rebuildRowItems();

  int listCount() const override { return static_cast<int>(files.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press BACK goes to root; short Back goes up a directory (home/cancel at
  // root), and Confirm activates on release while a hold opens file actions.
  bool handleCustomInput() override;
  bool handleButtons() override;
  // Header shows the current folder name (battery indicator via GUI.drawHeader);
  // footer labels depend on path depth and picker mode.
  void drawChrome() override;
  void drawFooter() override;
  void activateSelected();

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;
};
