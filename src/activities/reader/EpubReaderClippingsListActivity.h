#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "ClippingStore.h"

class EpubReaderClippingsListActivity final : public Activity {
  std::string bookPath;
  std::vector<ClippingEntry> clippings;
  std::vector<std::string> previewCache;  // Cached preview strings to avoid SD reads during render
  int selectorIndex = 0;
  bool confirmingDelete = false;

  void refreshPreviews();

  int getPageItems() const;
  int getTotalItems() const;

  void render(RenderLock&&) override;

 public:
  explicit EpubReaderClippingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& bookPath)
      : Activity("EpubReaderClippingsList", renderer, mappedInput), bookPath(bookPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
