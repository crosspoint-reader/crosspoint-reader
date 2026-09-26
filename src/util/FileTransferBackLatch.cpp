#include "FileTransferBackLatch.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalGPIO.h>

#include "BackTapDetector.h"

namespace {
constexpr uint32_t POLL_MS = 5;
constexpr uint32_t STACK_BYTES = 2048;
}  // namespace

bool FileTransferBackLatch::start(HalGPIO& input, const uint8_t physicalBack, const TapHook tapHook,
                                  void* const tapHookContext) {
  stop();
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) return true;
  gpio = &input;
  back = physicalBack;
  hook = tapHook;
  hookContext = tapHookContext;
  tapped.store(false, std::memory_order_relaxed);
  stopping.store(false, std::memory_order_relaxed);
  finished.store(false, std::memory_order_relaxed);
  if (xTaskCreate(run, "TransferBack", STACK_BYTES, this, 1, &task) != pdPASS) {
    task = nullptr;
    finished.store(true, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void FileTransferBackLatch::stop() {
  if (!task) return;
  stopping.store(true, std::memory_order_release);
  while (!finished.load(std::memory_order_acquire)) vTaskDelay(1);
  task = nullptr;
}

void FileTransferBackLatch::run(void* context) {
  auto* const self = static_cast<FileTransferBackLatch*>(context);
  BackTapDetector detector;
  while (!self->stopping.load(std::memory_order_acquire)) {
    InputManager::ButtonAdcSample first{}, second{};
    self->gpio->readButtonAdc(first, second);
    // Sampling the ladder while the main loop does can read zero on both pins
    // for a few ms; zero also classifies as a button, so drop that pair.
    const bool valid = first.raw >= 0 && second.raw >= 0 && !(first.raw == 0 && second.raw == 0);
    const bool down = first.button == self->back || second.button == self->back;
    if (detector.update(millis(), valid, down)) {
      self->tapped.store(true, std::memory_order_release);
      if (self->hook) self->hook(self->hookContext);
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
  self->finished.store(true, std::memory_order_release);
  // stop() may destroy the object from here on.
  vTaskDelete(nullptr);
}
