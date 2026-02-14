#pragma once

#include <functional>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// Enum for network mode selection
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

  const std::function<void(NetworkMode)> onModeSelected;
  const std::function<void()> onCancel;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void(NetworkMode)>& onModeSelected,
                                        const std::function<void()>& onCancel)
      : Activity("NetworkModeSelection", renderer, mappedInput), onModeSelected(onModeSelected), onCancel(onCancel) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
