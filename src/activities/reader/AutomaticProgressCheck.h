#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "KOReaderSyncClient.h"

/**
 * Runs an automatic KOReader progress check (pull) in a background FreeRTOS
 * task so the reader stays responsive while the network round-trip completes.
 *
 * Only the network fetch (WiFi connect + HTTP GET) runs off the main loop. The
 * position mapping and the prompt decision stay on the main loop, where the
 * reader owns the Epub and the renderer, so there is no shared-state race with
 * the render task.
 */
class AutomaticProgressCheck {
 public:
  enum class Status : uint8_t {
    IDLE,            // no check started
    RUNNING,         // network fetch in flight
    DONE_OK,         // remote progress fetched
    DONE_NOT_FOUND,  // server has no progress for this document
    DONE_ERROR,      // network / auth / other failure
  };

  AutomaticProgressCheck() = default;
  ~AutomaticProgressCheck();

  AutomaticProgressCheck(const AutomaticProgressCheck&) = delete;
  AutomaticProgressCheck& operator=(const AutomaticProgressCheck&) = delete;

  bool isRunning() const { return status_.load(std::memory_order_acquire) == Status::RUNNING; }

  /**
   * Spawns the background fetch for @p epubPath. Returns false when a check is
   * already running or no KOReader credentials are configured.
   */
  bool start(const std::string& epubPath);

  /** Non-blocking status poll for the main loop. */
  Status status() const { return status_.load(std::memory_order_acquire); }

  /** Completed remote progress; valid only when status() == DONE_OK. */
  const KOReaderProgress& remoteProgress() const { return remoteProgress_; }

  KOReaderSyncClient::Error error() const { return error_; }

  /** Returns the device to IDLE (call after consuming a result). */
  void reset() { status_.store(Status::IDLE, std::memory_order_release); }

 private:
  static void taskTrampoline(void* arg);
  void run();

  TaskHandle_t taskHandle_ = nullptr;
  std::atomic<Status> status_{Status::IDLE};
  std::string epubPath_;
  KOReaderProgress remoteProgress_;
  KOReaderSyncClient::Error error_ = KOReaderSyncClient::OK;
};
