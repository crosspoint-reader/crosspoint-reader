#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>

#include "../Activity.h"

// Enum for file transfer protocol selection
enum class FileTransferProtocol { HTTP, FTP };

/**
 * ProtocolSelectionActivity presents the user with a choice:
 * - "HTTP (Web Browser)" - Transfer files via web browser
 * - "FTP (File Client)" - Transfer files via FTP client
 *
 * The onProtocolSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class ProtocolSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int selectedIndex = 0;
  bool updateRequired = false;
  const std::function<void(FileTransferProtocol)> onProtocolSelected;
  const std::function<void()> onCancel;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

 public:
  explicit ProtocolSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void(FileTransferProtocol)>& onProtocolSelected,
                                     const std::function<void()>& onCancel)
      : Activity("ProtocolSelection", renderer, mappedInput), onProtocolSelected(onProtocolSelected), onCancel(onCancel) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
