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
#include <Epub/blocks/ImageBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>
#include <ZimImage.h>
#include <ZimLink.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "LibraryActivities.h"
#include "MappedInputManager.h"
#include "PictureActivity.h"
#include "PocketLibrary.h"
#include "ReaderFontSizes.h"
#include "ReadingHistory.h"
#include "SdCardFontSystem.h"
#include "SearchActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "icons/UprightIcon.h"
#include "icons/libraryIcons.h"

namespace {
constexpr const char* kWorkDir = "/.pocketlib";
constexpr const char* kXhtmlPath = "/.pocketlib/article.xhtml";
constexpr const char* kPagesPath = "/.pocketlib/article.pages";
// Layout work per loop pass once the first page is up; input is checked in
// between, so page turns stay responsive while the rest of a long article
// is laid out.
constexpr uint32_t kBuildSliceMs = 60;
// Link hit-testing, as in the EPUB reader: finger slop and a minimum width so
// a one-letter link is still tappable.
constexpr int kTouchSlop = 6;
constexpr int kMinTouchWidth = 28;
constexpr const char* kImageDir = "/.pocketlib/img";
constexpr const char* kImageBase = "/.pocketlib/img/";

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

std::string readable(std::string path) {
  for (char& c : path)
    if (c == '_') c = ' ';
  return path;
}
}  // namespace

ArticleActivity::ArticleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection,
                                 uint32_t entryIndex, std::string landing)
    : Activity("Article", renderer, mappedInput),
      collection_(collection),
      entry_(entryIndex),
      landing_(std::move(landing)) {}

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
  ImageBlock::getExtractor(bookExtractCtx_, bookExtractFn_);
  ImageBlock::setExtractor(this, &ArticleActivity::extractImage);
  const auto& cols = pocketlib::Library::instance().collections();
  if (collection_ < cols.size()) collectionKey_ = cols[collection_].key;
  openEntry(entry_, kNoOffset, landing_);
}

void ArticleActivity::onExit() {
  Activity::onExit();
  savePlace();
  resetLayout();
  page_.reset();  // ActivityManager holds the render lock around onExit()
  ImageBlock::releaseRenderCache();
  ImageBlock::setExtractor(bookExtractCtx_, bookExtractFn_);
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

void ArticleActivity::resetLayout() {
  if (parser_) parser_->abortParse();
  parser_.reset();
  building_ = false;
  if (pagesFile_) pagesFile_.close();
  pageOffsets_.clear();
  pageVisible_.clear();
  headings_.clear();
  anchors_.clear();
}

void ArticleActivity::openEntry(uint32_t entry, uint32_t offset, const std::string& fragment) {
  resetLayout();
  // "Images" lasts for the article it was asked on.
  if (!keepImageMode_) allImages_ = false;
  keepImageMode_ = false;
  entry_ = entry;
  targetOffset_ = offset;
  targetFragment_ = fragment;
  std::string title;
  if (zim::Archive* a = pocketlib::Library::instance().open(collection_)) {
    zim::Entry e;
    if (a->entryAt(entry, e) == zim::Error::None) title = e.title;  // one small read, for "Opening"
  }
  {
    RenderLock lock(*this);
    title_ = std::move(title);
    page_.reset();
    notice_.clear();
    error_.clear();
    currentPage_ = 0;
    shownTotal_ = 0;
    shownComplete_ = false;
  }
  loadAttempted_ = false;
  loadingShown_.store(false, std::memory_order_release);
  state_ = State::Loading;
  requestUpdate();
}

void ArticleActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (outlineReturn_ && state_ == State::Reading) {
      outlineReturn_ = false;
      openContents(true);  // a section chosen in the contents shown at open: back to them
    } else if (!back_.empty()) {
      goBack();  // also from "Opening" or an error: back to the article the link was on
    } else {
      finish();
    }
    return;
  }

  if (state_ == State::Loading) {
    // Load only after the "Opening" screen is up: the read and the layout
    // of the first page block this task for up to a few seconds.
    if (!loadAttempted_ && loadingShown_.load(std::memory_order_acquire)) {
      loadAttempted_ = true;
      int landing = 0;
      if (load()) {
        {
          RenderLock lock(*this);
          if (!targetFragment_.empty()) {
            const std::string anchor = anchorForFragment(targetFragment_);
            const int p = anchor.empty() ? -1 : pageForAnchor(anchor);
            if (p >= 0) landing = p;
          } else if (targetOffset_ != kNoOffset && targetOffset_ > 0) {
            const int p = pageForOffset(targetOffset_);
            if (p >= 0) landing = p;
          }
        }
        state_ = State::Reading;
        showPage(landing);
        savePlace();
        offerOutline_ = landing == 0 && pocketlib::ReadingHistory::instance().outlineByDefault();
      }
      requestUpdate();
    }
    return;
  }
  if (state_ != State::Reading) return;

  if (offerOutline_) {
    offerOutline_ = false;
    int sections = 0;
    for (const auto& h : headings_) sections += h.level >= 2 && !h.text.empty();
    if (sections >= 3) {
      openContents(true);
      return;
    }
  }

  if (handleLinkTap()) return;
  if (handlePictureTap()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openContents();
    return;
  }
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openContents();  // the contents, with the toolbar across the top
    return;
  }

  // Articles scroll like a web page: swipe up for the next page, down for the
  // previous one (the owner's choice; books keep their sideways swipes).
  // Sideways swipes do nothing here; taps and the side buttons turn as in books.
  bool swipeNext = false;
  bool swipePrev = false;
  ReaderUtils::TouchPageTurn touch{false, false, 0};
  const auto swipe = mappedInput.hasTouch() ? mappedInput.wasSwipe() : MappedInputManager::SwipeDir::None;
  if (swipe == MappedInputManager::SwipeDir::None) {
    touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  } else if (SETTINGS.touchReaderControls) {
    swipeNext = swipe == MappedInputManager::SwipeDir::Up;
    // A swipe down from the top edge belongs to the light panel and menus.
    swipePrev = swipe == MappedInputManager::SwipeDir::Down && !mappedInput.wasMenuGesture();
  }
  const auto turn = ReaderUtils::detectPageTurn(mappedInput);
  const bool prev = turn.prev || touch.prev || swipePrev;
  const bool next = turn.next || touch.next || swipeNext;
  if (next || prev) {
    const int target = currentPage_ + (next ? 1 : -1);
    if (target < 0) return;
    if (target >= static_cast<int>(pageOffsets_.size())) {
      if (!building_) return;  // last page: stay (Back leaves)
      // The next page isn't laid out yet: build until it is.
      RenderLock lock(*this);
      while (building_ && target >= static_cast<int>(pageOffsets_.size())) {
        if (!buildMore(kBuildSliceMs)) break;
      }
    }
    if (target >= static_cast<int>(pageOffsets_.size())) return;
    showPage(target);
    // The history file is rewritten on each save: on page turns, only every
    // few pages (leaving the article, or sleep, saves the exact page).
    if (++turnsSinceSave_ >= kTurnsPerSave) savePlace();
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
  err = archive->entryAt(entry_, entry);
  if (err == zim::Error::None) err = archive->resolve(entry);
  if (err != zim::Error::None) {
    fail(zim::errorName(err));
    return false;
  }
  if (entry.title != title_) {
    RenderLock lock(*this);  // a redirect resolves to its target's title
    title_ = entry.title;
  }
  article_ = entry;
  // Opened from a list (not a link or Back): resume where it was left.
  if (targetOffset_ == kNoOffset && targetFragment_.empty()) {
    uint32_t saved = 0;
    if (pocketlib::ReadingHistory::instance().find(collectionKey_, entry.ns, entry.path, saved)) targetOffset_ = saved;
  }
  const uint32_t t1 = millis();
  t.lookupMs = t1 - t0;

  const uint32_t missesBefore = archive->cacheStats().misses;
  // Straight from the decoded cluster when it is cached (no copy of the
  // article: a large one beside two ~2 MiB clusters can exhaust PSRAM).
  std::string storage;
  std::string_view html;
  err = archive->readView(entry, storage, html);
  if (err != zim::Error::None) {
    fail(zim::errorName(err));
    return false;
  }
  const uint32_t t2 = millis();
  t.readMs = t2 - t1;
  t.htmlBytes = static_cast<uint32_t>(html.size());
  t.clusterCached = archive->cacheStats().misses == missesBefore;

  Storage.ensureDirectoryExists(kWorkDir);
  // The previous article's pictures: their numbers would be reused.
  Storage.removeDir(kImageDir);
  Storage.ensureDirectoryExists(kImageDir);
  {
    HalFile out;
    if (!Storage.openFileForWrite("PLIB", kXhtmlPath, out)) {
      fail("Cannot write to the card");
      return false;
    }
    FileSink sink(out);
    zim::HtmlCleanOptions options;
    options.title = title_;
    options.keepLinks = true;
    options.headings = &headings_;
    lead_.clear();
    options.lead = &lead_;
    images_.clear();
    options.images = allImages_ ? zim::HtmlImages::All : zim::HtmlImages::None;  // pictures on request (Images)
    options.imageList = &images_;
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
  html = {};
  std::string().swap(storage);  // give the PSRAM back before layout
  // Layout needs room of its own. With little PSRAM left (seen on the
  // device: 816 KB free in 431 KB pieces after a 33-page article), drop the
  // decoded clusters too: Back then decodes again instead of the firmware
  // running out of memory mid-layout.
  constexpr size_t kLayoutReserve = 1536 * 1024;
  if (HalMemory::getPsramHeap().largestBlockBytes < kLayoutReserve) {
    LOG_INF("PLIB", "PSRAM low (largest %u KB): dropping cached clusters",
            static_cast<unsigned>(HalMemory::getPsramHeap().largestBlockBytes / 1024));
    archive->clearCache();
  }
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
  LOG_INF("PLIB", "PSRAM free %u KB (largest %u KB), internal free %u KB",
          static_cast<unsigned>(HalMemory::getPsramHeap().freeBytes / 1024),
          static_cast<unsigned>(HalMemory::getPsramHeap().largestBlockBytes / 1024),
          static_cast<unsigned>(HalMemory::getInternalHeap().freeBytes / 1024));
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
  static const std::string kImageBasePath = kImageBase;
  parser_ = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      nullptr, xhtmlPath_, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t visibleOffset) {
        const uint32_t pos = static_cast<uint32_t>(pagesFile_.position());
        if (page && page->serialize(pagesFile_)) {
          pageOffsets_.push_back(pos);
          pageVisible_.push_back(visibleOffset);
        }
      },
      /*embeddedStyle=*/false, kNoBase, kImageBasePath, /*imageRendering=*/0);
  if (!parser_) {
    fail("Out of memory for the layout engine");
    return false;
  }
  parser_->setTextSpacing(spec.characterSpacing, spec.wordSpacingPercent);
  if (!parser_->beginParse()) {
    parser_.reset();
    fail("Cannot read the cleaned article");
    return false;
  }
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
      a.refreshHeadingPages();
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
        anchors_ = parser_->getAnchors();
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
  anchors_ = parser_->getAnchors();
  parser_.reset();
  building_ = false;
  pagesFile_.flush();
  pocketlib::OpenTimings& t = pocketlib::Library::instance().lastOpen;
  t.allPagesMs = millis() - layoutStartMs_;
  t.pages = static_cast<uint16_t>(pageOffsets_.size());
  LOG_INF("PLIB", "laid out %u pages in %u ms", static_cast<unsigned>(t.pages), static_cast<unsigned>(t.allPagesMs));
}

int ArticleActivity::pageForAnchor(const std::string& anchor) {
  for (;;) {
    const auto& anchors = parser_ ? parser_->getAnchors() : anchors_;
    for (const auto& a : anchors) {
      if (a.first != anchor) continue;
      // The anchor's page may still be in progress: lay out until it exists.
      while (building_ && a.second >= pageOffsets_.size()) {
        if (!buildMore(kBuildSliceMs)) break;
      }
      const int last = static_cast<int>(pageOffsets_.size()) - 1;
      return std::min<int>(a.second, last);
    }
    if (!building_ || !buildMore(kBuildSliceMs)) break;
  }
  return -1;
}

int ArticleActivity::pageForOffset(uint32_t offset) {
  while (building_ && (pageVisible_.empty() || pageVisible_.back() <= offset)) {
    if (!buildMore(kBuildSliceMs)) break;
  }
  if (pageVisible_.empty()) return -1;
  int page = 0;
  for (size_t i = 0; i < pageVisible_.size() && pageVisible_[i] <= offset; i++) page = static_cast<int>(i);
  return page;
}

std::string ArticleActivity::anchorForFragment(const std::string& fragment) const {
  if (fragment.empty()) return {};
  for (size_t i = 0; i < headings_.size(); i++) {
    for (const auto& alias : headings_[i].aliases) {
      if (alias == fragment) return zim::headingAnchor(i);
    }
  }
  // Wikipedia ids are titles with underscores; a link may use spaces.
  const std::string underscored = [&] {
    std::string s = fragment;
    for (char& c : s)
      if (c == ' ') c = '_';
    return s;
  }();
  if (underscored != fragment) return anchorForFragment(underscored);
  const auto sameText = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
      if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  };
  for (size_t i = 0; i < headings_.size(); i++) {
    if (sameText(headings_[i].text, fragment)) return zim::headingAnchor(i);
  }
  // A landing asked for by a guide may name only the start of the heading
  // ("Heat" for "Heatstroke" or "Heat Stroke").
  if (fragment == landing_ && fragment.size() >= 3) {
    for (size_t i = 0; i < headings_.size(); i++) {
      if (headings_[i].text.size() >= fragment.size() &&
          sameText(headings_[i].text.substr(0, fragment.size()), fragment))
        return zim::headingAnchor(i);
    }
  }
  return {};
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
  notice_.clear();
}

void ArticleActivity::savePlace() {
  if (state_ != State::Reading || collectionKey_.empty() || article_.path.empty()) return;
  if (currentPage_ < 0 || currentPage_ >= static_cast<int>(pageVisible_.size())) return;
  pocketlib::Place place;
  place.collection = collectionKey_;
  place.ns = article_.ns;
  place.path = article_.path;
  place.title = title_;
  place.offset = pageVisible_[currentPage_];
  pocketlib::ReadingHistory::instance().record(place);
  turnsSinceSave_ = 0;
}

bool ArticleActivity::handleLinkTap() {
  if (!page_ || page_->links.empty() || !mappedInput.hasTouch()) return false;
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  const int pageX = x - marginLeft_;
  const int pageY = y - marginTop_;
  for (const auto& link : page_->links) {
    if (link.contains(pageX, pageY, kTouchSlop, kMinTouchWidth)) {
      const std::string href = link.href;  // page_ may be replaced below
      followLink(href.c_str());
      return true;
    }
  }
  return false;
}

// A tap on a picture shows it full screen.
bool ArticleActivity::handlePictureTap() {
  if (!page_ || !mappedInput.hasTouch()) return false;
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  for (const auto& el : page_->elements) {
    if (el->getTag() != TAG_PageImage) continue;
    const ImageBlock& img = static_cast<const PageImage&>(*el).getImageBlock();
    const int left = marginLeft_ + el->xPos;
    const int top = marginTop_ + el->yPos;
    if (x < left || x >= left + img.getWidth() || y < top || y >= top + img.getHeight()) continue;
    if (!img.imageExists()) return false;  // not drawn yet: nothing to enlarge
    auto picture = makeUniqueNoThrow<PictureActivity>(renderer, mappedInput, img.getImagePath());
    if (!picture) return false;
    startActivityForResult(std::move(picture), [this](const ActivityResult&) { requestUpdate(); });
    return true;
  }
  return false;
}

void ArticleActivity::followLink(const char* href) {
  zim::Archive* archive = pocketlib::Library::instance().open(collection_);
  if (!archive) return;
  zim::LinkTarget target;
  if (!zim::parseLink(article_, href, target)) return;

  const uint32_t here = currentPage_ < static_cast<int>(pageVisible_.size()) ? pageVisible_[currentPage_] : 0;
  auto remember = [&] {
    if (back_.size() >= kMaxBack) back_.erase(back_.begin());
    back_.push_back({article_.index, here});
  };

  zim::Entry to;
  bool samePage = target.samePage;
  if (!samePage) {
    zim::Error err = zim::Error::None;
    {
      // Drawing a page may be reading the archive for a picture.
      RenderLock lock(*this);
      err = archive->findByPath(target.ns, target.path, to);
      if (err == zim::Error::None) err = archive->resolve(to);
    }
    if (err != zim::Error::None) {
      LOG_INF("PLIB", "link %s: %s", href, zim::errorName(err));
      {
        RenderLock lock(*this);
        notice_ = "Not in this library: " + readable(target.path);
      }
      requestUpdate();
      return;
    }
    samePage = to.index == article_.index;
  }

  if (samePage) {
    const std::string anchor = anchorForFragment(target.fragment);
    int page = -1;
    if (!anchor.empty()) {
      RenderLock lock(*this);
      page = pageForAnchor(anchor);
    }
    if (page < 0) return;
    remember();
    showPage(page);
    savePlace();
    requestUpdate();
    return;
  }

  openLinked(to.index, target.fragment);
}

void ArticleActivity::openLinked(uint32_t entry, const std::string& fragment) {
  const uint32_t here = currentPage_ < static_cast<int>(pageVisible_.size()) ? pageVisible_[currentPage_] : 0;
  outlineReturn_ = false;
  savePlace();
  if (back_.size() >= kMaxBack) back_.erase(back_.begin());
  back_.push_back({article_.index, here});
  openEntry(entry, 0, fragment);
}

void ArticleActivity::goBack() {
  const Visit v = back_.back();
  back_.pop_back();
  if (v.entry == article_.index && state_ == State::Reading) {
    int page = -1;
    {
      RenderLock lock(*this);
      page = pageForOffset(v.offset);
    }
    if (page >= 0) {
      showPage(page);
      savePlace();
      requestUpdate();
    }
    return;
  }
  savePlace();
  openEntry(v.entry, v.offset, "");
}

void ArticleActivity::refreshHeadingPages() {
  headingPages_.assign(headings_.size(), -1);
  const auto& anchors = parser_ ? parser_->getAnchors() : anchors_;
  for (const auto& a : anchors) {
    if (a.first.compare(0, 4, "pl-h") != 0) continue;
    const size_t n = static_cast<size_t>(strtoul(a.first.c_str() + 4, nullptr, 10));
    if (n < headingPages_.size()) headingPages_[n] = a.second;
  }
}

int ArticleActivity::currentSection() const {
  int section = -1;
  for (size_t i = 0; i < headings_.size() && i < headingPages_.size(); i++) {
    if (headings_[i].level < 2 || headingPages_[i] < 0) continue;
    if (headingPages_[i] > currentPage_) break;
    section = static_cast<int>(i);
  }
  return section;
}

// The contents, after the Wikipedia app: the introduction (the article's
// opening sentences), then each section and subsection with its first
// sentence and its length; the one being read is marked and chosen. A section
// chosen from here: Back returns to the page left (or, when the contents
// opened with the article, to the contents). The last row turns "open
// articles at their contents" on or off.
void ArticleActivity::openContents(const bool atOpen) {
  std::vector<std::string> labels;
  std::vector<std::string> subtitles;
  std::vector<int> headingOf;  // -1 = the introduction, -2 = the setting
  int here = 0;
  {
    RenderLock lock(*this);
    const int current = currentSection();
    const int total = static_cast<int>(pageOffsets_.size());
    labels.emplace_back(current < 0 ? "\xE2\x80\xA2 Introduction" : "Introduction");
    subtitles.push_back(lead_.empty() ? std::string("Page 1") : zim::firstSentences(lead_, 2, 200));
    headingOf.push_back(-1);
    for (size_t i = 0; i < headings_.size(); i++) {
      const auto& h = headings_[i];
      if (h.level < 2 || h.level > 4 || h.text.empty()) continue;
      const bool isHere = static_cast<int>(i) == current;
      if (isHere) here = static_cast<int>(labels.size());
      labels.push_back(std::string(static_cast<size_t>(h.level - 2) * 3, ' ') + (isHere ? "\xE2\x80\xA2 " : "") +
                       h.text);
      // Length: up to the next heading of this level or higher.
      std::string length;
      const int page = i < headingPages_.size() ? headingPages_[i] : -1;
      if (page >= 0) {
        int endPage = shownComplete_ ? total : -1;
        for (size_t k = i + 1; k < headings_.size(); k++) {
          if (headings_[k].level <= h.level && k < headingPages_.size() && headingPages_[k] >= 0) {
            endPage = headingPages_[k];
            break;
          }
        }
        const int pages = endPage > page ? endPage - page : 1;
        length = std::to_string(pages) + (pages == 1 ? " page" : " pages");
      }
      std::string sub = isHere ? "You are here" : std::string();
      if (!h.summary.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + h.summary;
      if (!length.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + length;
      subtitles.push_back(std::move(sub));
      headingOf.push_back(static_cast<int>(i));
    }
  }
  const bool contentsFirst = pocketlib::ReadingHistory::instance().outlineByDefault();
  labels.emplace_back(contentsFirst ? "Open articles at their contents: On" : "Open articles at their contents: Off");
  subtitles.emplace_back("Tap to switch");
  headingOf.push_back(-2);

  auto list = makeUniqueNoThrow<ChoiceListActivity>(renderer, mappedInput, title_, std::move(labels),
                                                    std::move(subtitles), here);
  if (!list) return;
  // The reader's toolbar sits across the top of the contents.
  list->setToolbar({{icon_arrow_left_32_bits, back_.empty() ? "Close" : "Back"},
                    {icon_search_32_bits, "Search"},
                    {icon_a_large_small_32_bits, "Text size"},
                    {icon_image_32_bits, allImages_ ? "Hide images" : "Images"}});
  startActivityForResult(std::move(list), [this, headingOf, atOpen](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) return;
    const int row = std::get<MenuResult>(result.data).action;
    if (row <= ChoiceListActivity::kToolAction) {
      switch (ChoiceListActivity::kToolAction - row) {
        case 0:
          if (!back_.empty())
            goBack();
          else
            finish();
          break;
        case 1:
          openSearch();
          break;
        case 2:
          openTextSize();
          break;
        default:
          toggleImages();
          break;
      }
      return;
    }
    if (row < 0 || row >= static_cast<int>(headingOf.size())) return;
    if (headingOf[row] == -2) {
      auto& history = pocketlib::ReadingHistory::instance();
      history.setOutlineByDefault(!history.outlineByDefault());
      openContents(atOpen);
      return;
    }
    int page = 0;
    if (headingOf[row] >= 0) {
      RenderLock lock(*this);
      page = pageForAnchor(zim::headingAnchor(static_cast<size_t>(headingOf[row])));
    }
    if (page < 0) return;
    if (atOpen) {
      outlineReturn_ = true;  // Back returns to the contents
    } else {
      const uint32_t at = currentPage_ < static_cast<int>(pageVisible_.size()) ? pageVisible_[currentPage_] : 0;
      if (back_.size() >= kMaxBack) back_.erase(back_.begin());
      back_.push_back({article_.index, at});
      outlineReturn_ = false;
    }
    showPage(page);
    savePlace();
  });
}

// Text size: the point sizes the reader's font comes in (the same setting
// books use); the article is laid out again at the same place.
void ArticleActivity::openTextSize() {
  const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
  if (sizes.empty()) return;
  std::vector<std::string> labels;
  std::vector<std::string> subtitles;
  const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
  int current = 0;
  for (size_t i = 0; i < sizes.size(); i++) {
    if (sizes[i] == cur) current = static_cast<int>(i);
    labels.push_back(std::to_string(sizes[i]) + " pt");
    subtitles.push_back(sizes[i] == cur ? "Current size" : std::string());
  }
  auto list = makeUniqueNoThrow<ChoiceListActivity>(renderer, mappedInput, "Text size", std::move(labels),
                                                    std::move(subtitles), current);
  if (!list) return;
  startActivityForResult(std::move(list), [this, sizes](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) return;
    const int row = std::get<MenuResult>(result.data).action;
    if (row < 0 || row >= static_cast<int>(sizes.size()) || sizes[row] == SETTINGS.fontPointSize) return;
    SETTINGS.fontPointSize = sizes[row];
    SETTINGS.saveToFile();
    sdFontSystem.ensureLoaded(renderer);
    const uint32_t at = currentPage_ < static_cast<int>(pageVisible_.size()) ? pageVisible_[currentPage_] : 0;
    savePlace();
    keepImageMode_ = true;
    openEntry(article_.index, at, "");
  });
}

// Every picture, or back to the lead one: the article is laid out again at
// the same place.
void ArticleActivity::toggleImages() {
  allImages_ = !allImages_;
  const uint32_t here = currentPage_ < static_cast<int>(pageVisible_.size()) ? pageVisible_[currentPage_] : 0;
  savePlace();
  keepImageMode_ = true;
  openEntry(article_.index, here, "");
}

bool ArticleActivity::extractImage(void* ctx, const char* src, const char* dest) {
  auto* self = static_cast<ArticleActivity*>(ctx);
  const char* at = src ? strstr(src, "pl-img/") : nullptr;
  if (!self || !at) return false;
  const size_t n = static_cast<size_t>(strtoul(at + 7, nullptr, 10));
  if (n >= self->images_.size()) return false;
  zim::Archive* archive = pocketlib::Library::instance().open(self->collection_);
  zim::LinkTarget target;
  zim::Entry e;
  if (!archive || !zim::parseLink(self->article_, self->images_[n].src, target) ||
      archive->findByPath(target.ns, target.path, e) != zim::Error::None || archive->resolve(e) != zim::Error::None) {
    LOG_ERR("PLIB", "image %u (%s) not in the archive", static_cast<unsigned>(n), self->images_[n].src.c_str());
    return false;
  }
  const uint32_t t0 = millis();
  std::string storage;
  std::string_view bytes;
  if (archive->readView(e, storage, bytes) != zim::Error::None) return false;
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  const bool isWebp = bytes.size() >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
  const bool isPng = bytes.size() >= 8 && memcmp(data, "\x89PNG", 4) == 0;
  const bool isJpeg = bytes.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
  std::vector<uint8_t> png;
  zim::ImageSize size;
  const uint8_t* write = data;
  size_t writeSize = bytes.size();
  if (isWebp) {
    // Kiwix's WebP, often under its original .jpg or .png name (Wikipedia
    // since 2026): decoded, scaled and turned grey here (the reader has no
    // WebP decoder). The reader picks its decoder by the file's first bytes,
    // so a PNG under a .jpg name is shown.
    if (!zim::webpToGrayPng(data, bytes.size(), self->viewportWidth_, self->viewportHeight_ * 2 / 3, png, &size)) {
      LOG_ERR("PLIB", "image %u: not a WebP the decoder reads", static_cast<unsigned>(n));
      return false;
    }
    write = png.data();
    writeSize = png.size();
  } else if (!isJpeg && !isPng) {
    // A JPEG or PNG goes to the reader's own decoder as it is.
    LOG_ERR("PLIB", "image %u (%s): not a WebP, JPEG or PNG", static_cast<unsigned>(n), self->images_[n].src.c_str());
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("PLIB", dest, out)) return false;
  const bool ok = out.write(write, writeSize) == writeSize;
  out.flush();
  out.close();
  LOG_INF("PLIB", "image %u: %ux%u in %u ms", static_cast<unsigned>(n), static_cast<unsigned>(size.width),
          static_cast<unsigned>(size.height), static_cast<unsigned>(millis() - t0));
  return ok;
}

void ArticleActivity::openSearch() {
  auto search = makeUniqueNoThrow<SearchActivity>(renderer, mappedInput, static_cast<int>(collection_));
  if (!search) return;
  startActivityForResult(std::move(search), [this](const ActivityResult&) { requestUpdate(); });
}

void ArticleActivity::fail(const char* message) {
  // Free memory on the error screen: an out-of-memory report then says which
  // pool ran out (PSRAM holds clusters and article text, internal RAM the
  // layout's small allocations).
  const auto psram = HalMemory::getPsramHeap();
  const auto internal = HalMemory::getInternalHeap();
  char mem[96];
  snprintf(mem, sizeof(mem), "Free/largest: PSRAM %u/%u KB, RAM %u/%u KB",
           static_cast<unsigned>(psram.freeBytes / 1024), static_cast<unsigned>(psram.largestBlockBytes / 1024),
           static_cast<unsigned>(internal.freeBytes / 1024), static_cast<unsigned>(internal.largestBlockBytes / 1024));
  error_ = message ? message : "Error";
  memory_ = mem;
  state_ = State::Failed;
  building_ = false;
  LOG_ERR("PLIB", "article %u: %s. %s", static_cast<unsigned>(entry_), error_.c_str(), memory_.c_str());
}

void ArticleActivity::renderStatusBar() const {
  const int total = shownTotal_;
  const float progress = (total > 0 && shownComplete_) ? (currentPage_ + 1) * 100.0f / total : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    const int section = currentSection();
    title = section >= 0 ? headings_[section].text : title_;
  }
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
      renderer.drawCenteredText(UI_10_FONT_ID, y + 120, memory_.c_str());
      if (!back_.empty()) renderer.drawCenteredText(UI_10_FONT_ID, y + 160, "Back returns to the previous article");
    }
    if (!title_.empty()) {
      // Long titles ("MedlinePlus - Health Information from the National
      // Library of Medicine") wrap onto up to three lines.
      const int side = UITheme::getInstance().getMetrics().contentSidePadding;
      const auto lines =
          renderer.wrappedText(UI_10_FONT_ID, title_.c_str(), renderer.getScreenWidth() - 2 * side - 16, 3);
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      for (size_t i = 0; i < lines.size(); i++) {
        renderer.drawCenteredText(UI_10_FONT_ID, y + 40 + static_cast<int>(i) * lineHeight, lines[i].c_str());
      }
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    if (state_ == State::Loading) loadingShown_.store(true, std::memory_order_release);
    return;
  }
  if (!page_) return;

  const int fontId = SETTINGS.getReaderFontId();
  // A picture that failed on an earlier page (memory short while the layout
  // was still running) gets another try; otherwise it stays an empty box.
  ImageBlock::clearRenderFailures();
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
  if (!notice_.empty()) GUI.drawPopup(renderer, notice_.c_str());
}

#endif  // POCKET_LIBRARY
