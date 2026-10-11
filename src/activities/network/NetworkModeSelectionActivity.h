#pragma once

#include "NetworkMode.h"
#include "activities/UiListActivity.h"

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "OPDS Browser" - Download books from a catalog, opened directly (no result comes back);
 *   listed only while at least one OPDS server is configured
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "USB Drive" or "USB Transfer" - whichever USB transfer this board has, opened directly
 *   (no result comes back)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * Adapted from upstream crosspoint-reader's NetworkModeSelectionActivity (develop @ cdac66ffe,
 * src/activities/network/NetworkModeSelectionActivity.cpp). Ours lists the USB row on every
 * board (upstream only with USB mass storage), adds the OPDS browser, and keeps the serial
 * transfer, the direct hand-offs and usesWifi().
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  // `selected` is the row the screen opens on: the top one, or the row of a mode that left the
  // screen and came back (the OPDS browser). Not listed falls back to the top.
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        NetworkMode selected = NetworkMode::JOIN_NETWORK);

  void onEnter() override;

  // Every board lists four rows, the last one its USB transfer method, plus the OPDS browser
  // when a server is configured.
  static constexpr int MAX_ROWS = 5;

  bool usesWifi() const override { return true; }

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Label, subtitle and icon never change, so the rows are built once, in the constructor.
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  NetworkMode rowModes[MAX_ROWS]{};
  int rowCount = 0;
  NetworkMode selectedOnEnter;
};
