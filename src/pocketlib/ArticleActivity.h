// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <HalStorage.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class ChapterHtmlSlimParser;
class Page;

// Reads one article from the open collection.
//
// Opening: the entry's HTML is read from the ZIM (one cluster decode), cleaned
// into /.pocketlib/article.xhtml, and laid out by CrossPoint's own EPUB layout
// engine, so articles get the reader's fonts, margins, justification and
// hyphenation. Pages are written to /.pocketlib/article.pages as they are
// laid out; the first one is shown as soon as it exists and the rest are laid
// out between page turns.
class ArticleActivity final : public Activity {
 public:
  ArticleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection, uint32_t entryIndex);
  ~ArticleActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return state_.load() != State::Reading || building_; }
  bool skipLoopDelay() override { return building_; }

 private:
  enum class State : uint8_t { Loading, Reading, Failed };

  void computeViewport();
  bool load();
  bool startLayout();
  // Lays out more pages for at most `budgetMs`. Returns false on error.
  // Callers hold the render lock: layout measures text with the renderer.
  bool buildMore(uint32_t budgetMs);
  void finishLayout();
  std::unique_ptr<Page> loadPage(int index);
  void showPage(int index);
  void fail(const char* message);
  void renderStatusBar() const;

  const size_t collection_;
  const uint32_t entryIndex_;
  std::atomic<State> state_{State::Loading};
  std::atomic<bool> loadingShown_{false};
  bool loadAttempted_ = false;
  std::string title_;
  std::string error_;
  std::string memory_;  // free memory when error_ was set

  // Layout. The parser keeps a reference to xhtmlPath_.
  std::string xhtmlPath_;
  std::unique_ptr<ChapterHtmlSlimParser> parser_;
  HalFile pagesFile_;
  std::vector<uint32_t> pageOffsets_;
  bool building_ = false;
  uint32_t layoutStartMs_ = 0;

  // Display (guarded by RenderLock).
  std::unique_ptr<Page> page_;
  int currentPage_ = 0;
  int shownTotal_ = 0;          // pages laid out so far, as the status bar shows them
  bool shownComplete_ = false;  // layout finished: shownTotal_ is the article's length
  int pagesUntilFullRefresh_ = 0;
  int marginTop_ = 0, marginRight_ = 0, marginBottom_ = 0, marginLeft_ = 0;
  uint16_t viewportWidth_ = 0, viewportHeight_ = 0;
};
