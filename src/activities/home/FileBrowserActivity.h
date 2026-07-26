#pragma once

#include <I18n.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult;
  // PickFolder = same file listing as Books (so folders are distinguishable from books), plus
  // "Move here" / "New folder" rows on top; returns the chosen directory via FilePathResult
  // (used as the destination picker when moving a file).
  enum class Mode { Books, PickFirmware, PickFolder };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;
  // True after optionPopup (or a pushed ConfirmationActivity, e.g. promptDelete/promptNewFolder)
  // closed via a physical Back press: that press's release is still pending, and without this
  // guard we'd misread it as our own Back release and navigate up a directory / go home on top of
  // whatever the popup/dialog already did.
  bool lockNextBackRelease = false;

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Long-press entry menu (Books mode): rename (folders) / move / delete / new folder.
  OptionPopup optionPopup;

  // Timed feedback popup (e.g. "Move failed").
  StrId popupMsgId = StrId::STR_MOVE_FAILED;
  bool popupVisible = false;
  unsigned long popupTime = 0;

  // Folder-move progress: BookMover::moveFolder blocks the main task while the
  // render task draws a percentage bar from these fields (same pattern as
  // SdFirmwareUpdateActivity's flash progress).
  bool moveInProgress = false;
  size_t moveDone = 0;
  size_t moveTotal = 0;
  unsigned int lastRenderedPercent = 101;

  // Runs BookMover::moveFolder with the percentage-bar progress screen up.
  bool moveFolderWithProgress(const std::string& srcPath, const std::string& dstPath);

  // Rows prepended to the list in PickFolder mode ("Move here", "New folder").
  size_t syntheticCount() const { return mode == Mode::PickFolder ? 2 : 0; }

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  void showFileMenu(const std::string& entry);
  void promptDelete(const std::string& entry, const std::string& fullPath);
  void promptMoveDestination(const std::string& srcPath, bool isDirectory);
  void promptRenameFolder(const std::string& srcPath);
  void promptNewFolder();
  void showMessage(StrId msgId);

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
