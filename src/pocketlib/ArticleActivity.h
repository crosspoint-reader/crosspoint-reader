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
#include <ZimArchive.h>
#include <ZimHtml.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"

class ChapterHtmlSlimParser;
class Page;

// Reads articles from the open collection.
//
// Opening: the entry's HTML is read from the ZIM (one cluster decode), cleaned
// into /.pocketlib/article.xhtml, and laid out by CrossPoint's own EPUB layout
// engine, so articles get the reader's fonts, margins, justification and
// hyphenation. Pages are written to /.pocketlib/article.pages as they are
// laid out; the first one is shown as soon as it exists and the rest are laid
// out between page turns.
//
// Reading, after the Wikipedia app: tap a link for a preview card (its
// article's first sentences; tap the card to open it in this screen); Back
// returns through the articles followed, to the place left in each. A tap in
// the middle of the page shows a toolbar (Back, Contents, Search, Outline);
// Confirm opens the contents, which mark the section being read and give
// each section's page. The status bar names the section. The outline is the
// lead and the section headings: choose one to read it, Back returns to the
// outline; articles can open in it (a setting at its foot). Where each
// article was left is saved (ReadingHistory) and restored on the next open.
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

  static constexpr size_t kMaxBack = 32;

 private:
  enum class State : uint8_t { Loading, Reading, Failed };
  static constexpr uint32_t kNoOffset = UINT32_MAX;

  struct Visit {
    uint32_t entry;
    uint32_t offset;  // visible-text offset of the page being read
  };

  void computeViewport();
  // Starts loading `entry` in this screen. `offset` (kNoOffset = saved place
  // or top) and `fragment` (a #anchor from a link) choose the first page.
  void openEntry(uint32_t entry, uint32_t offset, const std::string& fragment);
  void resetLayout();
  bool load();
  bool startLayout();
  // Lays out more pages for at most `budgetMs`. Returns false on error.
  // Callers hold the render lock: layout measures text with the renderer.
  bool buildMore(uint32_t budgetMs);
  void finishLayout();
  // Page holding an anchor id or a text offset, laying out as far as needed
  // (render lock held by the caller). -1 when not found.
  int pageForAnchor(const std::string& anchor);
  int pageForOffset(uint32_t offset);
  // "#History" -> "pl-h3" through the headings' aliases; "" if unknown.
  std::string anchorForFragment(const std::string& fragment) const;
  std::unique_ptr<Page> loadPage(int index);
  void showPage(int index);
  void savePlace();
  bool handleLinkTap();
  void followLink(const char* href);
  void goBack();
  void openContents();
  void openOutline();
  void openSearch();
  // The overlays drawn over the page: the toolbar and the link preview.
  bool handleOverlayInput();
  void showPreview(const zim::Entry& to, const std::string& fragment);
  void openLinked(uint32_t entry, const std::string& fragment);
  void hideOverlays();
  void drawToolbar() const;
  void drawPreview() const;
  // Page of each heading (-1 while not laid out yet), from the layout's
  // anchors; refreshed as layout runs. Callers hold the render lock.
  void refreshHeadingPages();
  void toggleImages();
  // ImageBlock's extractor: makes /.pocketlib/img/<n>.png from the article's
  // n-th picture (a WebP in the archive) when its page is first drawn.
  static bool extractImage(void* ctx, const char* src, const char* dest);
  int currentSection() const;  // heading index, or -1 before the first section
  void fail(const char* message);
  void renderStatusBar() const;

  const size_t collection_;
  uint32_t entry_;      // directory index as asked for (may be a redirect)
  zim::Entry article_;  // the resolved content entry being read
  std::string collectionKey_;
  std::atomic<State> state_{State::Loading};
  std::atomic<bool> loadingShown_{false};
  bool loadAttempted_ = false;
  std::string title_;
  std::string error_;
  std::string memory_;  // free memory when error_ was set
  std::string notice_;  // one-line message over the page ("Not in this library"), cleared on the next turn

  // Where to land once the article is laid out.
  uint32_t targetOffset_ = kNoOffset;
  std::string targetFragment_;
  std::vector<Visit> back_;

  // Layout. The parser keeps a reference to xhtmlPath_.
  std::string xhtmlPath_;
  std::unique_ptr<ChapterHtmlSlimParser> parser_;
  HalFile pagesFile_;
  std::vector<uint32_t> pageOffsets_;  // file offset of each page
  std::vector<uint32_t> pageVisible_;  // visible-text offset where each page starts
  std::vector<zim::HtmlHeading> headings_;
  std::vector<int> headingPages_;
  std::string lead_;                    // the article's opening paragraphs, for the outline
  std::vector<zim::HtmlImage> images_;  // pictures kept by the cleaner, by number
  bool allImages_ = false;              // this article with every picture, not just the lead one
  bool keepImageMode_ = false;          // the next openEntry is that same article again
  void* bookExtractCtx_ = nullptr;      // a book's image extractor, put back on exit
  bool (*bookExtractFn_)(void*, const char*, const char*) = nullptr;
  bool outlineReturn_ = false;  // a section chosen in the outline: Back returns to it
  bool offerOutline_ = false;   // open the outline once the first page is up
  bool toolbar_ = false;        // guarded by RenderLock
  struct Preview {
    bool shown = false;
    uint32_t entry = 0;
    std::string fragment;
    std::string title;
    std::string text;
  } preview_;                                              // guarded by RenderLock
  mutable std::atomic<int> previewTop_{0};                 // set when the card is drawn, read by taps
  std::vector<std::pair<std::string, uint16_t>> anchors_;  // copied from the parser when layout ends
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
