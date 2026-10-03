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

#include <FreeInkUIIcon.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>

#include "ArticleActivity.h"
#include "MappedInputManager.h"
#include "PocketLibrary.h"
#include "ReadingHistory.h"
#include "SearchActivity.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "icons/libraryIcons.h"

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

bool locatePlace(const pocketlib::Place& place, size_t& collection, uint32_t& entry) {
  auto& lib = pocketlib::Library::instance();
  const auto& cols = lib.collections();
  for (size_t i = 0; i < cols.size(); i++) {
    if (cols[i].key != place.collection) continue;
    zim::Archive* a = lib.open(i);
    zim::Entry e;
    if (!a || a->findByPath(place.ns, place.path, e) != zim::Error::None) return false;
    collection = i;
    entry = e.index;
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ shelf

ShelfActivity::ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string group)
    : UiListActivity("PocketShelf", renderer, mappedInput), group_(std::move(group)) {}

std::string tileGroupFor(const pocketlib::Collection& c) {
  if (!c.group.empty()) return c.group;
  const std::string& k = c.key;
  if (k.rfind("wikipedia", 0) == 0) return "Wikipedia";
  for (const char* medical : {"mdwiki", "medlineplus", "medicine", "wikem", "medical"}) {
    if (k.find(medical) != std::string::npos) return "Medical";
  }
  return "More";
}

namespace {
const freeink::Icon* iconForGroup(const std::string& group) {
  if (group == "Wikipedia") return &icon_globe_32;
  if (group == "Medical") return &icon_heart_pulse_32;
  if (group == "More") return &icon_ellipsis_32;
  return &icon_book_open_32;
}

const freeink::Icon* iconForCollection(const std::string& key) {
  static const struct {
    const char* key;
    const freeink::Icon* icon;
  } kIcons[] = {
      {"wikipedia", &icon_globe_32},    {"wiktionary", &icon_book_a_32},      {"wikivoyage", &icon_plane_32},
      {"wikiquote", &icon_quote_32},    {"wikisource", &icon_scroll_text_32}, {"wikibooks", &icon_graduation_cap_32},
      {"mdwiki", &icon_stethoscope_32}, {"medlineplus", &icon_pill_32},       {"military", &icon_shield_plus_32},
  };
  for (const auto& i : kIcons) {
    if (key.find(i.key) != std::string::npos) return i.icon;
  }
  return &icon_book_open_32;
}
}  // namespace

void ShelfActivity::onEnter() {
  auto& lib = pocketlib::Library::instance();
  if (lib.collections().empty()) lib.load();
  title_ = group_.empty() ? "Library" : group_;
  rebuildTiles();
  UiListActivity::onEnter();
}

void ShelfActivity::rebuildTiles() {
  const auto& cols = pocketlib::Library::instance().collections();
  tiles_.clear();
  if (group_.empty()) {
    tiles_.push_back({TileKind::Recent, "Recent", &icon_clock_32, -1, ""});
    tiles_.push_back({TileKind::Books, "eBooks", &icon_book_open_32, -1, ""});
    // Wikipedia, Maps, Medical, More; then any other group the manifest names.
    std::vector<std::string> groups = {"Wikipedia", "Maps", "Medical", "More"};
    for (const auto& c : cols) {
      const std::string g = tileGroupFor(c);
      if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    for (const auto& g : groups) {
      if (g == "Maps") {
        tiles_.push_back({TileKind::Maps, "Maps", &icon_map_32, -1, ""});
        continue;
      }
      int only = -1;
      int members = 0;
      for (size_t i = 0; i < cols.size(); i++) {
        if (tileGroupFor(cols[i]) != g) continue;
        only = static_cast<int>(i);
        members++;
      }
      if (members == 0) continue;
      // A group of one is that collection, under the group's name.
      if (members == 1)
        tiles_.push_back({TileKind::Collection, g, iconForGroup(g), only, ""});
      else
        tiles_.push_back({TileKind::Group, g, iconForGroup(g), -1, g});
    }
  } else {
    for (size_t i = 0; i < cols.size(); i++) {
      if (tileGroupFor(cols[i]) == group_)
        tiles_.push_back(
            {TileKind::Collection, cols[i].title, iconForCollection(cols[i].key), static_cast<int>(i), ""});
    }
  }

  const bool anyRecent = !pocketlib::ReadingHistory::instance().places().empty();
  items_.assign(tiles_.size(), fui::TileGridItem{});
  for (size_t i = 0; i < tiles_.size(); i++) {
    items_[i].label = tiles_[i].label.c_str();
    items_[i].icon = fui::bitmapFromIcon(*tiles_[i].icon);
    items_[i].value = static_cast<int16_t>(i);
    items_[i].enabled = tiles_[i].kind != TileKind::Recent || anyRecent;
  }
}

void ShelfActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t top = static_cast<int16_t>(metrics.topPadding + metrics.headerHeight);
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  screen.setContentMarginFromScreen(fui::Insets{top, side, static_cast<int16_t>(metrics.buttonHintsHeight), side});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  constexpr int16_t kGap = 12;
  const int rows = std::max<int>(3, (static_cast<int>(tiles_.size()) + 1) / 2);
  const int available =
      renderer.getScreenHeight() - top - metrics.buttonHintsHeight - 2 * metrics.verticalSpacing - (rows - 1) * kGap;
  for (size_t i = 0; i < items_.size(); i++) {
    // Filled = the tile the buttons are on (no dithered gray, so no ghosting).
    items_[i].state = buttonsUsed_ && static_cast<int>(i) == nav.selected ? fui::StateChecked : fui::StateNormal;
  }
  fui::TileGridProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.columns = 2;
  props.gap = kGap;
  props.tileHeight = static_cast<int16_t>(std::max(48, available / rows));
  props.iconSize = 32;
  props.text = screen.theme().bodyText;
  screen.tileGrid(props);
}

void ShelfActivity::navigateButtons() {
  const int count = listCount();
  if (count == 0) return;
  auto move = [this, count](int delta) {
    if (!buttonsUsed_) {
      buttonsUsed_ = true;  // the first press shows where the selection is
    } else {
      nav.selected = (nav.selected + delta + count) % count;
    }
    notice_.clear();
    requestUpdate();
  };
  buttonNavigator.onNextRelease([move] { move(1); });
  buttonNavigator.onPreviousRelease([move] { move(-1); });
}

void ShelfActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (!notice_.empty()) GUI.drawPopup(renderer, notice_.c_str());
}

void ShelfActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(tiles_.size())) return;
  if (!items_[index].enabled) return;
  notice_.clear();
  const Tile& tile = tiles_[index];
  switch (tile.kind) {
    case TileKind::Books:
      activityManager.goToLibrary();  // CrossPoint's book library
      return;
    case TileKind::Recent:
      openRecent();
      return;
    case TileKind::Maps:
      notice_ = "Offline maps are coming in a later update";
      requestUpdate();
      return;
    case TileKind::Group: {
      auto sub = makeUniqueNoThrow<ShelfActivity>(renderer, mappedInput, tile.group);
      if (sub) startActivityForResult(std::move(sub), [this](const ActivityResult&) { rebuildTiles(); });
      return;
    }
    case TileKind::Collection:
      openCollection(static_cast<size_t>(tile.collection));
      return;
  }
}

void ShelfActivity::openCollection(size_t collection) {
  auto activity = makeUniqueNoThrow<CollectionActivity>(renderer, mappedInput, collection);
  if (!activity) {
    LOG_ERR("PLIB", "OOM: collection activity");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { rebuildTiles(); });
}

void ShelfActivity::openRecent() {
  const auto& places = pocketlib::ReadingHistory::instance().places();
  const auto& cols = pocketlib::Library::instance().collections();
  std::vector<std::string> labels;
  std::vector<std::string> subs;
  for (const auto& p : places) {
    labels.push_back(p.title);
    std::string where = p.collection;
    for (const auto& c : cols)
      if (c.key == p.collection) where = c.title;
    subs.push_back(std::move(where));
  }
  auto list =
      makeUniqueNoThrow<ChoiceListActivity>(renderer, mappedInput, "Recent", std::move(labels), std::move(subs));
  if (!list) return;
  startActivityForResult(std::move(list), [this](const ActivityResult& result) {
    if (result.isCancelled || !std::holds_alternative<MenuResult>(result.data)) return;
    const int row = std::get<MenuResult>(result.data).action;
    const auto& places = pocketlib::ReadingHistory::instance().places();
    if (row < 0 || row >= static_cast<int>(places.size())) return;
    const pocketlib::Place place = places[row];
    size_t collection = 0;
    uint32_t entry = 0;
    if (locatePlace(place, collection, entry)) {
      auto article = makeUniqueNoThrow<ArticleActivity>(renderer, mappedInput, collection, entry);
      if (article) startActivityForResult(std::move(article), [this](const ActivityResult&) { rebuildTiles(); });
      return;
    }
    LOG_ERR("PLIB", "recent article %s no longer on the card", place.path.c_str());
  });
}

void ShelfActivity::onBackButton() {
  if (group_.empty())
    onGoHome(HomeMenuItem::LIBRARY);
  else
    finish();
}

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
    subtitles_[ROW_ABOUT] = pocketlib::formatCount(articles) + " titles" +
                            (lib.titleIndex(collection_) ? ", search index OK" : ", no search index");
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
  auto search = makeUniqueNoThrow<SearchActivity>(renderer, mappedInput, static_cast<int>(collection_));
  if (!search) return;
  startActivityForResult(std::move(search), [this](const ActivityResult&) { refreshRows(); });
}

void CollectionActivity::showMessage(const std::string& message) {
  subtitles_[ROW_MAIN] = message;
  refreshRows();
  requestUpdate();
}

// ------------------------------------------------------------ choice list

ChoiceListActivity::ChoiceListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                       std::vector<std::string> labels, std::vector<std::string> subtitles)
    : UiListActivity("PocketChoice", renderer, mappedInput),
      title_(std::move(title)),
      labels_(std::move(labels)),
      subtitles_(std::move(subtitles)) {}

void ChoiceListActivity::onEnter() {
  items_.assign(labels_.size(), fui::ListItem{});
  for (size_t i = 0; i < items_.size(); i++) {
    items_[i].label = labels_[i].c_str();
    items_[i].subtitle = i < subtitles_.size() && !subtitles_[i].empty() ? subtitles_[i].c_str() : nullptr;
    items_[i].actionValue = static_cast<int16_t>(i);
  }
  UiListActivity::onEnter();
}

void ChoiceListActivity::buildScreen(UiScreen& screen) {
  fui::ListProps props;
  layoutList(screen, props);
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}

void ChoiceListActivity::activateIndex(int index) {
  MenuResult r;
  r.action = index;
  setResult(std::move(r));
  finish();
}

void ChoiceListActivity::onBackButton() {
  ActivityResult r;
  r.isCancelled = true;
  setResult(std::move(r));
  finish();
}

#endif  // POCKET_LIBRARY
