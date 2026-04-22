#pragma once
#include "../Activity.h"
#include "WriterDraftStore.h"

class WriterActivity final : public Activity {
  WriterDraftStore draftStore;

 public:
  explicit WriterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Writer", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
