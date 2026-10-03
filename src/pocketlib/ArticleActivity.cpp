// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "ArticleActivity.h"

#include <Epub/Page.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <ZimHtml.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PocketLibrary.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kWorkDir = "/.pocketlib";
constexpr const char* kXhtmlPath = "/.pocketlib/article.xhtml";
constexpr const char* kPagesPath = "/.pocketlib/article.pages";
// Layout work per loop pass once the first page is up; input is checked in
// between, so page turns stay responsive while the rest of a long article
// is laid out.
constexpr uint32_t kBuildSliceMs = 60;

// Streams the cleaner's output to the card.
class FileSink final : public zim::HtmlSink {
 public:
  explicit FileSink(HalFile& f) : f_(f) {}
  bool write(const char* data, size_t len) override {
    return f_.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  }

 private:
  HalFile& f_;
};
}  // namespace

ArticleActivity::ArticleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection,
                                 uint32_t entryIndex)
    : Activity("Article", renderer, mappedInput), collection_(collection), entryIndex_(entryIndex) {}

ArticleActivity::~ArticleActivity() {
  if (parser_) parser_->abortParse();
  parser_.reset();
  if (pagesFile_) pagesFile_.close();
}

void ArticleActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.ensureLoaded(renderer);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  computeViewport();
  // Title for the loading screen; reading the dirent is one small SD read.
  if (zim::Archive* a = pocketlib::Library::instance().open(collection_)) {
    zim::Entry e;
    if (a->entryAt(entryIndex_, e) == zim::Error::None) title_ = e.title;
  }
  requestUpdate();
}

void ArticleActivity::onExit() {
  Activity::onExit();
  if (parser_) parser_->abortParse();
  parser_.reset();
  building_ = false;
  if (pagesFile_) pagesFile_.close();
  page_.reset();  // ActivityManager holds the render lock around onExit()
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void ArticleActivity::computeViewport() {
  renderer.getOrientedViewableTRBL(&marginTop_, &marginRight_, &marginBottom_, &marginLeft_);
  marginTop_ += SETTINGS.screenMargin;
  marginLeft_ += SETTINGS.screenMargin;
  marginRight_ += SETTINGS.screenMargin;
  marginBottom_ += std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));
  viewportWidth_ = static_cast<uint16_t>(renderer.getScreenWidth() - marginLeft_ - marginRight_);
  viewportHeight_ = static_cast<uint16_t>(renderer.getScreenHeight() - marginTop_ - marginBottom_);
}

void ArticleActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state_ == State::Loading) {
    // Load only after the "Opening" screen is up: the read and the layout
    // of the first page block this task for up to a few seconds.
    if (!loadAttempted_ && loadingShown_.load(std::memory_order_acquire)) {
      loadAttempted_ = true;
      if (load()) {
        state_ = State::Reading;
        showPage(0);
      }
      requestUpdate();
    }
    return;
  }
  if (state_ != State::Reading) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  const auto turn = ReaderUtils::detectPageTurn(mappedInput);
  const bool prev = turn.prev || touch.prev;
  const bool next = turn.next || touch.next;
  if (next || prev) {
    const int target = currentPage_ + (next ? 1 : -1);
    if (target < 0) return;
    if (target >= static_cast<int>(pageOffsets_.size())) {
      if (!building_) {
        finish();  // past the last page: back to the collection
        return;
      }
      // The next page isn't laid out yet: build until it is.
      RenderLock lock(*this);
      while (building_ && target >= static_cast<int>(pageOffsets_.size())) {
        if (!buildMore(kBuildSliceMs)) break;
      }
    }
    if (target >= static_cast<int>(pageOffsets_.size())) return;
    showPage(target);
    requestUpdate();
    return;
  }

  if (building_) {
    // Background layout, only while the screen task is idle.
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      buildMore(kBuildSliceMs);
      if (!building_) {
        // Repaint once so the status bar shows the final page count.
        lock.unlock();
        requestUpdate();
      }
    }
  }
}

bool ArticleActivity::load() {
  pocketlib::OpenTimings& t = pocketlib::Library::instance().lastOpen;
  t = {};
  const uint32_t t0 = millis();

  zim::Error err = zim::Error::None;
  zim::Archive* archive = pocketlib::Library::instance().open(collection_, &err);
  if (!archive) {
    fail(zim::errorName(err));
    return false;
  }
  zim::Entry entry;
  err = archive->entryAt(entryIndex_, entry);
  if (err == zim::Error::None) err = archive->resolve(entry);
  if (err != zim::Error::None) {
    fail(zim::errorName(err));
    return false;
  }
  if (entry.title != title_) {
    RenderLock lock(*this);  // a redirect resolves to its target's title
    title_ = entry.title;
  }
  const uint32_t t1 = millis();
  t.lookupMs = t1 - t0;

  const uint32_t missesBefore = archive->cacheStats().misses;
  std::string html;
  err = archive->read(entry, html);
  if (err != zim::Error::None) {
    fail(zim::errorName(err));
    return false;
  }
  const uint32_t t2 = millis();
  t.readMs = t2 - t1;
  t.htmlBytes = static_cast<uint32_t>(html.size());
  t.clusterCached = archive->cacheStats().misses == missesBefore;

  Storage.ensureDirectoryExists(kWorkDir);
  {
    HalFile out;
    if (!Storage.openFileForWrite("PLIB", kXhtmlPath, out)) {
      fail("Cannot write to the card");
      return false;
    }
    FileSink sink(out);
    zim::HtmlCleanOptions options;
    options.title = title_;
    zim::HtmlCleanStats stats;
    const bool ok = zim::cleanArticleHtml(html, options, sink, &stats);
    out.flush();
    out.close();
    if (!ok) {
      fail("Card full or write error");
      return false;
    }
    t.cleanBytes = static_cast<uint32_t>(stats.outputBytes);
  }
  std::string().swap(html);  // give the PSRAM back before layout
  const uint32_t t3 = millis();
  t.cleanMs = t3 - t2;

  if (!startLayout()) return false;
  {
    RenderLock lock(*this);
    while (building_ && pageOffsets_.empty()) {
      if (!buildMore(1000)) break;
    }
  }
  if (pageOffsets_.empty()) {
    if (state_ != State::Failed) fail(building_ ? "Layout failed" : "The article is empty");
    return false;
  }
  t.firstPageMs = millis() - t3;
  t.valid = true;
  LOG_INF("PLIB", "open \"%s\": lookup %u ms, read %u ms (%u B, cluster %s), clean %u ms (%u B), first page %u ms",
          title_.c_str(), static_cast<unsigned>(t.lookupMs), static_cast<unsigned>(t.readMs),
          static_cast<unsigned>(t.htmlBytes), t.clusterCached ? "cached" : "read", static_cast<unsigned>(t.cleanMs),
          static_cast<unsigned>(t.cleanBytes), static_cast<unsigned>(t.firstPageMs));
  return true;
}

bool ArticleActivity::startLayout() {
  if (!Storage.openFileForWrite("PLIB", kPagesPath, pagesFile_)) {
    fail("Cannot write to the card");
    return false;
  }
  Hyphenator::setPreferredLanguage("en");
  xhtmlPath_ = kXhtmlPath;
  const ReaderRenderSpec spec = SETTINGS.readerRenderSpec(viewportWidth_, viewportHeight_);
  static const std::string kNoBase;
  parser_ = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      nullptr, xhtmlPath_, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
        const uint32_t pos = static_cast<uint32_t>(pagesFile_.position());
        if (page && page->serialize(pagesFile_)) pageOffsets_.push_back(pos);
      },
      /*embeddedStyle=*/false, kNoBase, kNoBase, /*imageRendering=*/1);
  if (!parser_) {
    fail("Out of memory");
    return false;
  }
  parser_->setTextSpacing(spec.characterSpacing, spec.wordSpacingPercent);
  if (!parser_->beginParse()) {
    parser_.reset();
    fail("Cannot read the cleaned article");
    return false;
  }
  pageOffsets_.clear();
  building_ = true;
  layoutStartMs_ = millis();
  return true;
}

bool ArticleActivity::buildMore(uint32_t budgetMs) {
  if (!building_ || !parser_) return false;
  const uint32_t start = millis();
  struct Publish {
    ArticleActivity& a;
    ~Publish() {
      a.shownTotal_ = static_cast<int>(a.pageOffsets_.size());
      a.shownComplete_ = !a.building_;
    }
  } publish{*this};
  do {
    switch (parser_->parseStep()) {
      case ChapterHtmlSlimParser::ParseStatus::More:
        break;
      case ChapterHtmlSlimParser::ParseStatus::Done:
        finishLayout();
        return true;
      case ChapterHtmlSlimParser::ParseStatus::Error:
        LOG_ERR("PLIB", "layout error after %u pages", static_cast<unsigned>(pageOffsets_.size()));
        parser_->abortParse();
        parser_.reset();
        building_ = false;
        pagesFile_.flush();
        return false;
    }
  } while (millis() - start < budgetMs);
  return true;
}

void ArticleActivity::finishLayout() {
  parser_->finishParse();  // emits the last page
  parser_.reset();
  building_ = false;
  pagesFile_.flush();
  pocketlib::OpenTimings& t = pocketlib::Library::instance().lastOpen;
  t.allPagesMs = millis() - layoutStartMs_;
  t.pages = static_cast<uint16_t>(pageOffsets_.size());
  LOG_INF("PLIB", "laid out %u pages in %u ms", static_cast<unsigned>(t.pages), static_cast<unsigned>(t.allPagesMs));
}

std::unique_ptr<Page> ArticleActivity::loadPage(int index) {
  if (index < 0 || index >= static_cast<int>(pageOffsets_.size()) || !pagesFile_) return nullptr;
  // The page file is open read/write for the layout: read the page, then put
  // the write position back so layout keeps appending.
  const size_t writePos = pagesFile_.position();
  pagesFile_.flush();
  pagesFile_.seek(pageOffsets_[index]);
  auto page = Page::deserialize(pagesFile_);
  pagesFile_.seek(writePos);
  return page;
}

void ArticleActivity::showPage(int index) {
  auto page = loadPage(index);
  if (!page) {
    fail("Cannot read the laid-out page");
    return;
  }
  RenderLock lock(*this);
  page_ = std::move(page);
  currentPage_ = index;
}

void ArticleActivity::fail(const char* message) {
  error_ = message ? message : "Error";
  state_ = State::Failed;
  building_ = false;
  LOG_ERR("PLIB", "article %u: %s", static_cast<unsigned>(entryIndex_), error_.c_str());
}

void ArticleActivity::renderStatusBar() const {
  const int total = shownTotal_;
  const float progress = (total > 0 && shownComplete_) ? (currentPage_ + 1) * 100.0f / total : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) title = title_;
  GUI.drawStatusBar(renderer, progress, currentPage_ + 1, total, title);
}

void ArticleActivity::render(RenderLock&&) {
  if (state_ == State::Loading || state_ == State::Failed) {
    renderer.clearScreen();
    const int y = renderer.getScreenHeight() / 3;
    if (state_ == State::Loading) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "Opening", true, EpdFontFamily::BOLD);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "Could not open this article", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, y + 80, error_.c_str());
    }
    if (!title_.empty()) renderer.drawCenteredText(UI_10_FONT_ID, y + 40, title_.c_str());
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (state_ == State::Loading) loadingShown_.store(true, std::memory_order_release);
    return;
  }
  if (!page_) return;

  const int fontId = SETTINGS.getReaderFontId();
  renderer.clearScreen();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page_->render(renderer, fontId, marginLeft_, marginTop_);
  renderStatusBar();
  scope.endScanAndPrewarm();

  page_->render(renderer, fontId, marginLeft_, marginTop_);
  renderStatusBar();
  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh_);
    ReaderUtils::renderAntiAliased(renderer,
                                   [this, fontId]() { page_->render(renderer, fontId, marginLeft_, marginTop_); });
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh_);
  }
}

#endif  // POCKET_LIBRARY
