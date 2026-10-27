#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, OPDS_BROWSER };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Calibre Wireless" - Use Calibre wireless device transfers
 * - "OPDS Browser" - Browse OPDS catalogs (only when a server is configured)
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "USB Drive" - Expose the SD card over USB (boards with USB mass storage)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

#if FREEINK_CAP_USB_MSC
  static constexpr int MAX_ITEM_COUNT = 5;
#else
  static constexpr int MAX_ITEM_COUNT = 4;
#endif

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Row storage: built once in the constructor instead of every buildScreen()
  // call, into fixed-capacity storage that avoids any heap allocation for the
  // row list. actionValue is the row index; rowModes_ maps it to a mode.
  freeink::ui::ListItem rowItems_[MAX_ITEM_COUNT]{};
  NetworkMode rowModes_[MAX_ITEM_COUNT]{};
  int rowCount_ = 0;
};
