#pragma once
#include <esp_partition.h>

class RecoveryMode final {
  const esp_partition_t* appPartition = nullptr;
  const esp_partition_t* recoveryPartition = nullptr;

  bool requestRender = true;

  enum class State {
    ERROR_SDCARD,
    IDLE,
    FLASHING,
  } state = State::IDLE;

 public:
  void setup();
  void loop();
  void render();
};
