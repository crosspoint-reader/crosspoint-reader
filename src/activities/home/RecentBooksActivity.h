#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/BookSearchUtils.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool confirmPressSeen = false;
  bool confirmLongHandled = false;
  bool suppressPopupConfirmRelease = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<bool> pinnedFlags;
  OptionPopup optionPopup;

  bool searchActive = false;
  bool searchResultsTruncated = false;
  std::string searchQuery;
  std::vector<size_t> searchResults;
  StrId popupMessage = StrId::STR_NONE_OPT;
  unsigned long popupTime = 0;

  // Data loading
  void loadRecentBooks();
  size_t visibleItemCount() const;
  bool isSearchRow(size_t index) const;
  size_t sourceIndex(size_t visibleIndex) const;
  void launchSearch();
  void applySearch(const std::string& query);
  void clearSearch(bool preserveQuery = false);
  void showBookActions(size_t sourceIndex);

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override {
    return !optionPopup.isActive() && handleSafeGlobalShortcut(shortcut);
  }
};
