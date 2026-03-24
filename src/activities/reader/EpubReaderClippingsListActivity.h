#pragma once
#include <Epub.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "ClippingStore.h"
#include "util/ButtonNavigator.h"

class EpubReaderClippingsListActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string bookPath;
  std::vector<ClippingEntry> clippings;
  std::vector<std::string> previewCache;
  int selectorIndex = 0;
  bool confirmingDelete = false;
  ButtonNavigator buttonNavigator;

  void refreshPreviews();

 public:
  explicit EpubReaderClippingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::shared_ptr<Epub> epub, const std::string& bookPath)
      : Activity("EpubReaderClippingsList", renderer, mappedInput), epub(std::move(epub)), bookPath(bookPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
