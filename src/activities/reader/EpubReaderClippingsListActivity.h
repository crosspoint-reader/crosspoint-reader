#pragma once
#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "ClippingStore.h"

class EpubReaderClippingsListActivity final : public ActivityWithSubactivity {
  std::string bookPath;
  std::vector<ClippingEntry> clippings;
  std::vector<std::string> previewCache;  // Cached preview strings to avoid SD reads during render
  int selectorIndex = 0;
  bool confirmingDelete = false;

  void refreshPreviews();

  const std::function<void()> onGoBack;

  int getPageItems() const;
  int getTotalItems() const;

  void render(Activity::RenderLock&&) override;

 public:
  explicit EpubReaderClippingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& bookPath, const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("EpubReaderClippingsList", renderer, mappedInput),
        bookPath(bookPath),
        onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
