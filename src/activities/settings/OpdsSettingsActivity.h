#pragma once

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"

/**
 * Edit screen for a single OPDS server.
 * Shows Name, URL, Username, Password fields and a Delete option.
 * Used for both adding new servers and editing existing ones.
 */
class OpdsSettingsActivity final : public UiListActivity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1);

  void onEnter() override;

 private:
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;

  int listCount() const override { return getMenuItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  int getMenuItemCount() const;
  void handleSelection();
  bool saveServer();

  // Row storage: at most 5 rows (Name/URL/Username/Password + Delete, see
  // BASE_ITEMS in the .cpp), so a fixed-capacity array avoids any heap
  // allocation for the row list. Labels are set once in the constructor
  // (they never change); buildScreen() only refreshes the value pointers,
  // which already point at editServer's own fields (no new strings built).
  static constexpr int MAX_MENU_ITEMS = 5;
  freeink::ui::ListItem fieldRowItems[MAX_MENU_ITEMS]{};
};
