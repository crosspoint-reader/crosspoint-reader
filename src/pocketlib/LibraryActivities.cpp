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

#include "LibraryActivities.h"

#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <cstdio>

#include "ArticleActivity.h"
#include "MappedInputManager.h"
#include "PocketLibrary.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
void layoutList(UiAppHost::UiScreen& screen, fui::ListProps& props) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
}
}  // namespace

// ------------------------------------------------------------------ shelf

ShelfActivity::ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("PocketShelf", renderer, mappedInput) {}

void ShelfActivity::onEnter() {
  auto& lib = pocketlib::Library::instance();
  if (lib.collections().empty()) lib.load();
  const auto& cols = lib.collections();

  // One row per collection, then the book library. Strings live in members
  // so the ListItems can point at them.
  subtitles_.clear();
  values_.clear();
  for (const auto& c : cols) {
    std::string sub = c.description;
    if (!c.date.empty()) sub += sub.empty() ? c.date : " \xC2\xB7 " + c.date;  // " · "
    subtitles_.push_back(std::move(sub));
    values_.push_back(pocketlib::formatBytes(c.bytes));
  }
  if (cols.empty()) {
    subtitles_.push_back(lib.loadError().empty() ? "Build the card with cardbuilder.py" : lib.loadError());
    values_.emplace_back();
  }
  subtitles_.push_back("EPUBs and other books on this card");
  values_.emplace_back();

  items_.assign(subtitles_.size(), fui::ListItem{});
  for (size_t i = 0; i < items_.size(); i++) {
    const bool books = i + 1 == items_.size();
    items_[i].label = books ? "Books" : (cols.empty() ? "No collections found" : cols[i].title.c_str());
    items_[i].subtitle = subtitles_[i].c_str();
    items_[i].value = values_[i].c_str();
    items_[i].actionValue = static_cast<int16_t>(i);
    items_[i].enabled = books || !cols.empty();
  }
  UiListActivity::onEnter();
}

int ShelfActivity::listCount() const { return static_cast<int>(items_.size()); }

void ShelfActivity::buildScreen(UiScreen& screen) {
  fui::ListProps props;
  layoutList(screen, props);
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}

void ShelfActivity::activateIndex(int index) {
  const auto& cols = pocketlib::Library::instance().collections();
  if (index == static_cast<int>(items_.size()) - 1) {
    activityManager.goToLibrary();  // CrossPoint's book library
    return;
  }
  if (index < 0 || index >= static_cast<int>(cols.size())) return;
  auto activity = makeUniqueNoThrow<CollectionActivity>(renderer, mappedInput, static_cast<size_t>(index));
  if (!activity) {
    LOG_ERR("PLIB", "OOM: collection activity");
    return;
  }
  startActivityForResult(std::move(activity), nullptr);
}

void ShelfActivity::onBackButton() { onGoHome(HomeMenuItem::LIBRARY); }

// ------------------------------------------------------------- collection

CollectionActivity::CollectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection)
    : UiListActivity("PocketCollection", renderer, mappedInput), collection_(collection) {}

void CollectionActivity::onEnter() {
  const auto& cols = pocketlib::Library::instance().collections();
  if (collection_ < cols.size()) title_ = cols[collection_].title;
  static const char* const kLabels[ROW_COUNT] = {"Search", "Main page", "Random article", "About", "Last article"};
  for (int i = 0; i < ROW_COUNT; i++) {
    items_[i].label = kLabels[i];
    items_[i].actionValue = static_cast<int16_t>(i);
  }
  refreshRows();
  UiListActivity::onEnter();
}

void CollectionActivity::refreshRows() {
  auto& lib = pocketlib::Library::instance();
  zim::Error err = zim::Error::None;
  zim::Archive* a = lib.open(collection_, &err);
  if (!a) {
    subtitles_[ROW_ABOUT] = std::string("Cannot open: ") + zim::errorName(err);
  } else {
    const uint32_t articles = a->hasArticleList() ? a->articleListCount() : a->titleCount();
    subtitles_[ROW_ABOUT] =
        pocketlib::formatCount(articles) + " titles" + (lib.titleIndex() ? ", search index OK" : ", no search index");
  }
  const pocketlib::OpenTimings& t = lib.lastOpen;
  if (t.valid) {
    char buf[160];
    snprintf(buf, sizeof(buf), "read %u ms%s \xC2\xB7 clean %u ms \xC2\xB7 first page %u ms%s", t.readMs,
             t.clusterCached ? " (cached)" : "", t.cleanMs, t.firstPageMs, "");
    subtitles_[ROW_LAST_OPEN] = buf;
    if (t.pages > 0) {
      snprintf(buf, sizeof(buf), " \xC2\xB7 %u pages in %.1f s", t.pages, t.allPagesMs / 1000.0);
      subtitles_[ROW_LAST_OPEN] += buf;
    }
  } else {
    subtitles_[ROW_LAST_OPEN] = "Open an article to see timings";
  }
  if (lib.lastSearchMs > 0) {
    subtitles_[ROW_SEARCH] = "Titles as you type \xC2\xB7 last lookup " + std::to_string(lib.lastSearchMs) + " ms";
  } else {
    subtitles_[ROW_SEARCH] = "Titles as you type";
  }
  for (int i = 0; i < ROW_COUNT; i++) {
    items_[i].subtitle = subtitles_[i].empty() ? nullptr : subtitles_[i].c_str();
    items_[i].value = values_[i].empty() ? nullptr : values_[i].c_str();
    items_[i].enabled = i < ROW_ABOUT && a != nullptr;
  }
}

void CollectionActivity::buildScreen(UiScreen& screen) {
  fui::ListProps props;
  layoutList(screen, props);
  props.items = items_;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}

void CollectionActivity::activateIndex(int index) {
  switch (index) {
    case ROW_MAIN:
      openMain();
      break;
    case ROW_RANDOM:
      openRandom();
      break;
    case ROW_SEARCH:
      openSearch();
      break;
    default:
      break;
  }
}

void CollectionActivity::openEntry(uint32_t entryIndex) {
  auto activity = makeUniqueNoThrow<ArticleActivity>(renderer, mappedInput, collection_, entryIndex);
  if (!activity) {
    LOG_ERR("PLIB", "OOM: article activity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    // Back from an article: show its timings.
    refreshRows();
  });
}

void CollectionActivity::openMain() {
  zim::Archive* a = pocketlib::Library::instance().open(collection_);
  if (!a) return;
  zim::Entry e;
  const zim::Error err = a->mainEntry(e);
  if (err != zim::Error::None) {
    showMessage(std::string("No main page: ") + zim::errorName(err));
    return;
  }
  openEntry(e.index);
}

void CollectionActivity::openRandom() {
  zim::Archive* a = pocketlib::Library::instance().open(collection_);
  if (!a) return;
  zim::Entry e;
  if (a->hasArticleList()) {
    const uint32_t pos = esp_random() % a->articleListCount();
    if (a->articleListEntryAt(pos, e) == zim::Error::None) {
      openEntry(e.index);
      return;
    }
  }
  // No article list: try random entries until one is an HTML article.
  const char ns = a->contentNamespace();
  for (int tries = 0; tries < 64; tries++) {
    if (a->entryAt(esp_random() % a->entryCount(), e) != zim::Error::None) continue;
    if (e.ns != ns) continue;
    if (e.isContent() && a->mimeType(e.mime).rfind("text/html", 0) != 0) continue;
    openEntry(e.index);
    return;
  }
  showMessage("No random article found");
}

void CollectionActivity::openSearch() {
  if (!pocketlib::Library::instance().open(collection_)) return;
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "Search " + title_, lastQuery_, 64,
                                                           InputType::Text);
  if (!keyboard) return;
  // Runs on the main task after every edit; the keyboard shows the rows that
  // fit and reports which one was chosen by its position.
  keyboard->setLiveSuggestions([this](const std::string& text, std::vector<std::string>& rows, std::string& status) {
    liveEntries_.clear();
    if (text.empty()) {
      status = "Type the start of a title";
      return;
    }
    std::vector<pocketlib::Library::Hit> hits;
    pocketlib::Library::instance().search(text, 8, hits);
    for (auto& h : hits) {
      rows.push_back(std::move(h.title));
      liveEntries_.push_back(h.entry);
    }
    if (rows.empty()) status = "No title starts with \"" + text + "\"";
  });
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      refreshRows();
      return;
    }
    const auto& kb = std::get<KeyboardResult>(result.data);
    lastQuery_ = kb.text;
    if (kb.picked >= 0 && kb.picked < static_cast<int>(liveEntries_.size())) {
      openEntry(liveEntries_[kb.picked]);
    } else {
      refreshRows();
    }
  });
}

void CollectionActivity::showMessage(const std::string& message) {
  subtitles_[ROW_MAIN] = message;
  refreshRows();
  requestUpdate();
}

#endif  // POCKET_LIBRARY
