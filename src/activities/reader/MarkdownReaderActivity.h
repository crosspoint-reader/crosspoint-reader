#pragma once

#include <Epub/Section.h>
#include <Markdown.h>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class MarkdownReaderActivity final : public Activity {
  std::unique_ptr<Markdown> md;
  std::unique_ptr<Section> section;

  int currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool initialized = false;

  void initializeReader();
  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit MarkdownReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Markdown> md)
      : Activity("MdReader", renderer, mappedInput), md(std::move(md)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
