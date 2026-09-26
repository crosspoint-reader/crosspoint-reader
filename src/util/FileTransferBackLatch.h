#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

class HalGPIO;

// Samples the physical Back button on its own task while File Transfer runs.
// WebServer reads a whole HTTP upload inside one handleClient() call, so the
// main loop, and with it normal input, stops for as long as an upload lasts.
class FileTransferBackLatch {
 public:
  using TapHook = void (*)(void* context);

  ~FileTransferBackLatch() { stop(); }

  // Starts nothing on boards without the Xteink button ladder. `hook` runs on
  // the sampler task when a tap latches, so it must be safe from another task.
  // False when the task could not be created.
  bool start(HalGPIO& input, uint8_t physicalBack, TapHook hook, void* hookContext);
  // Returns once the sampler task no longer touches this object.
  void stop();
  // True once for a tap latched since start().
  bool consume() { return tapped.exchange(false, std::memory_order_acquire); }

 private:
  static void run(void* context);

  TaskHandle_t task = nullptr;
  HalGPIO* gpio = nullptr;
  uint8_t back = 0;
  TapHook hook = nullptr;
  void* hookContext = nullptr;
  std::atomic<bool> tapped{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> finished{true};
};
