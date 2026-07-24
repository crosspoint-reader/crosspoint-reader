#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/BookSearchUtils.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);
  void promptDelete(const std::string& fullPath, const std::string& entry);
  void showBookActions(const std::string& fullPath, const std::string& entry, bool isDirectory);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;
  bool confirmPressSeen = false;
  bool confirmLongHandled = false;
  bool suppressPopupConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;
  OptionPopup optionPopup;

  bool searchActive = false;
  bool searchResultsTruncated = false;
  std::string searchQuery;
  std::vector<size_t> searchResults;
  StrId popupMessage = StrId::STR_NONE_OPT;
  unsigned long popupTime = 0;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;
  size_t visibleItemCount() const;
  bool isSearchRow(size_t index) const;
  const std::string* visibleEntry(size_t index) const;
  void launchSearch();
  void applySearch(const std::string& query);
  void clearSearch(bool preserveQuery = false);

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
  bool handleGlobalShortcut(GlobalShortcut shortcut) override {
    return mode == Mode::Books && !optionPopup.isActive() && handleSafeGlobalShortcut(shortcut);
  }
};
