#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "EndOfBookOptions.h"
#include "ReaderDocument.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;
  bool allowFastInitialRefresh;
  int pagesUntilFullRefresh = 0;
  bool forcedRefreshPending = false;

  std::unique_ptr<ReaderDocument> document;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};

  std::unique_ptr<ReaderDocument> createDocument(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isImageFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToBmpViewer(const std::string& path);
  void onGoBack();
  int initialRefreshCountdown() const;

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          bool allowFastInitialRefresh)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        allowFastInitialRefresh(allowFastInitialRefresh),
        pagesUntilFullRefresh(initialRefreshCountdown()) {}
  ~ReaderActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

  bool isReaderActivity() const override { return true; }
  bool appliesNightMode() const override;
  bool skipLoopDelay() override;
  bool handleForcedRefresh() override;
  ScreenshotInfo getScreenshotInfo() const override;

  GfxRenderer& getRenderer() { return renderer; }
  MappedInputManager& getMappedInput() { return mappedInput; }
  void onGoHome() { Activity::onGoHome(); }
};
