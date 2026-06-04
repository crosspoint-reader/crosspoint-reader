#pragma once
#include <RssParser.h>

#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"

class RssArticleActivity final : public Activity {
 public:
  RssArticleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, RssItem item)
      : Activity("RssArticle", renderer, mappedInput), item(std::move(item)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  static constexpr int MAX_ARTICLE_LINES = 512;

  RssItem item;
  std::vector<std::string> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  int viewportWidth = 0;
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  int fontId = 0;
  int pagesUntilFullRefresh = 1;
  bool initialized = false;

  void initializeLayout();
  void buildLines();
  void renderPage();
  std::string articleText() const;
};
