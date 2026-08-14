#pragma once

#include <memory>
#include <string>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "util/ScreenshotInfo.h"

class ReaderActivity;

struct ReaderRenderContext {
  GfxRenderer& renderer;
  int& pagesUntilFullRefresh;
  bool& forcedRefreshPending;
  RenderLock& lock;
};

class ReaderDocument {
 protected:
  ReaderActivity& host;

 public:
  explicit ReaderDocument(ReaderActivity& host) : host(host) {}
  virtual ~ReaderDocument() = default;

  // Metadata
  virtual const std::string& getPath() const = 0;
  virtual std::string getTitle() const = 0;
  virtual std::string getAuthor() const { return ""; }
  virtual std::string getThumbBmpPath() const { return ""; }

  // Lifecycle & Processing
  virtual bool load(bool allowFastInitialRefresh) = 0;
  virtual void loop() {}
  virtual void render(ReaderRenderContext& context) = 0;

  // Navigation
  virtual bool pageTurn(bool isForward) = 0;
  virtual bool skipPages(int amount) { return pageTurn(amount > 0); }
  virtual bool isAtEndOfBook() const = 0;
  virtual void onReturnFromEndOfBook() {}

  // Power & Build scheduling
  virtual bool skipLoopDelay() const { return false; }
  virtual bool appliesNightMode() const { return true; }

  // Display & Status Bar Ownership
  virtual bool rendersOwnStatusBar() const { return false; }
  virtual bool commitsDisplayBuffer() const { return false; }
  virtual void renderStatusBar(GfxRenderer& renderer) const {}

  // Screenshots
  virtual ScreenshotInfo getScreenshotInfo() const = 0;
};
