#pragma once
#include <Epub.h>

#include <memory>

#include "../../HighlightEntry.h"
#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderHighlightsActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  std::vector<HighlightEntry> highlights;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete

 public:
  explicit EpubReaderHighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : Activity("EpubReaderHighlights", renderer, mappedInput), epub(epub), epubPath(epubPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the highlight list based on orientation
  int getListHeight(const GfxRenderer& renderer);
};
