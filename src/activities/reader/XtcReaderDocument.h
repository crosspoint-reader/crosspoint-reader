#pragma once

#include <Xtc.h>

#include <memory>
#include <string>

#include "ReaderDocument.h"

class XtcReaderDocument final : public ReaderDocument {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage(ReaderRenderContext& context);
  void openChapterSelection();
  void renderStatusBarOverlay(GfxRenderer& renderer, StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderDocument(ReaderActivity& host, std::unique_ptr<Xtc> xtc)
      : ReaderDocument(host), xtc(std::move(xtc)) {}
  ~XtcReaderDocument() override = default;

  const std::string& getPath() const override {
    static const std::string empty;
    return xtc ? xtc->getPath() : empty;
  }
  std::string getTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }

  bool load(bool allowFastInitialRefresh) override;
  void loop() override;
  void render(ReaderRenderContext& context) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool rendersOwnStatusBar() const override { return true; }
  bool commitsDisplayBuffer() const override { return true; }

  ScreenshotInfo getScreenshotInfo() const override;
};
