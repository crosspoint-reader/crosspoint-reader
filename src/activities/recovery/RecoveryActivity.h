#pragma once
#include "../Activity.h"

#include <esp_partition.h>

class RecoveryActivity final : public Activity {
  const esp_partition_t *appPartition = nullptr;
  const esp_partition_t *recoveryPartition = nullptr;

 public:
  explicit RecoveryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Recovery", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
