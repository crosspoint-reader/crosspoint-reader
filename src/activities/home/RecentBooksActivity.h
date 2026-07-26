#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // True after the ConfirmationActivity pushed by promptRemoveBook closed via a physical Back
  // press (to cancel): that press's release is still pending and must not be reinterpreted as
  // our own Back release, which would immediately go home on top of just closing the dialog.
  bool lockNextBackRelease = false;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Where to put the home selector when Back returns home. Set when this
  // activity was opened by the home Back shortcut, which must leave the home
  // selection where the user left it instead of moving it onto "Recent Books".
  const HomeMenuItem homeReturnItem;
  const int homeReturnRecentIndex;

  // Data loading
  void loadRecentBooks();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               HomeMenuItem homeReturnItemValue = HomeMenuItem::NONE,
                               int homeReturnRecentIndexValue = -1)
      : Activity("RecentBooks", renderer, mappedInput),
        homeReturnItem(homeReturnItemValue),
        homeReturnRecentIndex(homeReturnRecentIndexValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
