#pragma once

#include <string>

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"
#include "activities/browser/OpdsBackTo.h"

// Adapted from upstream crosspoint-reader's OpdsServerListActivity (develop @ cdac66ffe,
// src/activities/settings/OpdsServerListActivity.cpp).
//
// The list of configured OPDS servers. Settings mode lists the servers, then "Add Server",
// "Download folder" and "Filename format"; a server row opens its editor. Picker mode (from File
// Transfer, or a finished book's author search) lists the servers only, and a server row opens the
// OPDS browser on that server, carrying the search query and the Back destination it was opened
// with.
class OpdsServerListActivity final : public UiListActivity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false,
                                  OpdsBackTo backTo = OpdsBackTo::FileTransfer, std::string initialQuery = {});

  void onEnter() override;

 private:
  // Settings mode appends "Add Server", "Download folder" and "Filename format" to the servers.
  static constexpr int SETTINGS_ROWS = 3;
  static constexpr int MAX_ROWS = static_cast<int>(OpdsServerStore::MAX_SERVERS) + SETTINGS_ROWS;

  int listCount() const override { return getItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Picker mode backs out to backTo rather than finishing: it was opened by replacing, so there is
  // no parent to finish to.
  void onBackButton() override;
  // Long Back in picker mode goes Home; the base would return to a stale parent.
  void homeFromList() override;
  const char* headerTitle() const override;

  int serverRows() const;
  int getItemCount() const;
  int rebuildRowItems();
  void reloadServers();
  void handleSelection(int index);

  bool pickerMode = false;
  OpdsBackTo backTo;
  std::string initialQuery_;
  // The rows: pointers into OPDS_STORE's strings, SETTINGS and the string table. rebuildRowItems()
  // refills them at the start of every build, so only the render task writes them, and they can
  // never point into a store the server editor has changed since they were built. At most
  // MAX_SERVERS + 3 rows, so a fixed array and no heap.
  freeink::ui::ListItem rowItems_[MAX_ROWS]{};
};
