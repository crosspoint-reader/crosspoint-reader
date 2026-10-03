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
#include "fontIds.h"
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

std::string shortTitle(const pocketlib::Collection& c) {
  static const struct {
    const char* key;
    const char* name;
  } kNames[] = {{"mdwiki", "MDWiki"}, {"medlineplus", "MedlinePlus"}, {"military-medicine", "Military Medicine"}};
  for (const auto& n : kNames) {
    if (c.key.find(n.key) != std::string::npos) return n.name;
  }
  std::string t = c.title;
  for (const char* sep : {" - ", " \xE2\x80\x93 ", ": ", " ("}) {  // " – " is an en dash
    const size_t at = t.find(sep);
    if (at != std::string::npos && at > 0) t.resize(at);
  }
  return t;
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
            {TileKind::Collection, shortTitle(cols[i]), iconForCollection(cols[i].key), static_cast<int>(i), ""});
    }
  }

  const bool anyRecent = !pocketlib::ReadingHistory::instance().places().empty();
  items_.assign(tiles_.size(), fui::TileGridItem{});
  for (size_t i = 0; i < tiles_.size(); i++) {
    items_[i].value = static_cast<int16_t>(i);
    items_[i].enabled = tiles_[i].kind != TileKind::Recent || anyRecent;
  }
}

void ShelfActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  const int16_t top = static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing);
  const int16_t bottom =
      static_cast<int16_t>(renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing);

  constexpr int16_t kGap = 12;
  const int rows = std::max<int>(3, (static_cast<int>(tiles_.size()) + 1) / 2);
  tileHeight_ = static_cast<int16_t>(std::max(48, (bottom - top - (rows - 1) * kGap) / rows));
  gridRect_ = fui::Rect{side, top, static_cast<int16_t>(renderer.getScreenWidth() - 2 * side),
                        static_cast<int16_t>(rows * tileHeight_ + (rows - 1) * kGap)};
  for (size_t i = 0; i < items_.size(); i++) {
    items_[i].state = buttonsUsed_ && static_cast<int>(i) == nav.selected ? fui::StateChecked : fui::StateNormal;
  }
  // Outlined tiles; the one the buttons are on gets a heavy border instead of
  // a fill, so its black icon and name stay readable (and nothing is gray).
  fui::StyleSet styles = fui::tileGridStyles(12);
  fui::BoxStyle chosen = styles.normal;
  chosen.borderWidth = 5;
  styles.selected = chosen;
  styles.focused = chosen;
  styles.active = chosen;
  fui::TileGridProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.columns = 2;
  props.gap = kGap;
  props.tileHeight = tileHeight_;
  props.styles = styles;
  fui::tileGrid(screen.frame(), gridRect_, props);
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
  // Icon above, name below, centred in each tile and cut to its width.
  constexpr int kGap = 12;
  constexpr int kIcon = 32;
  const int tileWidth = (gridRect_.width - kGap) / 2;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  for (size_t i = 0; i < tiles_.size(); i++) {
    const int x = gridRect_.x + static_cast<int>(i % 2) * (tileWidth + kGap);
    const int y = gridRect_.y + static_cast<int>(i / 2) * (tileHeight_ + kGap);
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, tiles_[i].label.c_str(), tileWidth - 20, 2);
    const int blockHeight = kIcon + 10 + static_cast<int>(lines.size()) * lineHeight;
    int ty = y + (tileHeight_ - blockHeight) / 2;
    renderer.drawIcon(tiles_[i].icon->bits, x + (tileWidth - kIcon) / 2, ty, kIcon);
    ty += kIcon + 10;
    const bool enabled = items_[i].enabled;
    for (const auto& line : lines) {
      const int w = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
      renderer.drawText(UI_12_FONT_ID, x + (tileWidth - w) / 2, ty, line.c_str(), true,
                        enabled ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      ty += lineHeight;
    }
  }
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
  if (collection_ < cols.size()) {
    title_ = shortTitle(cols[collection_]);
    icon_ = iconForCollection(cols[collection_].key);
  }
  searchLabel_ = "Search " + title_;
  refreshRows();
  UiListActivity::onEnter();
  app.on(
      ACTION_SEARCH_BAR,
      [](const fui::ActionEvent&, void* user) { static_cast<CollectionActivity*>(user)->openSearch(); }, this);
}

void CollectionActivity::refreshRows() {
  auto& lib = pocketlib::Library::instance();
  const auto& cols = lib.collections();
  rows_.clear();
  labels_.clear();
  subtitles_.clear();
  rows_.push_back({RowKind::Search, 0});
  auto add = [&](RowKind kind, size_t place, std::string label, std::string sub) {
    rows_.push_back({kind, place});
    labels_.push_back(std::move(label));
    subtitles_.push_back(std::move(sub));
  };

  // Continue reading, then up to three more recent articles from here.
  const std::string key = collection_ < cols.size() ? cols[collection_].key : std::string();
  const auto& places = pocketlib::ReadingHistory::instance().places();
  int shown = 0;
  for (size_t i = 0; i < places.size() && shown < 4; i++) {
    if (places[i].collection != key) continue;
    add(RowKind::Place, i, places[i].title, shown == 0 ? "Continue reading" : "Recent");
    shown++;
  }

  zim::Error err = zim::Error::None;
  zim::Archive* a = lib.open(collection_, &err);
  const std::string description = collection_ < cols.size() ? cols[collection_].description : std::string();
  add(RowKind::Main, 0, "Main page", description);
  add(RowKind::Random, 0, "Random article", "");

  std::string about;
  if (!a) {
    about = std::string("Cannot open: ") + zim::errorName(err);
  } else {
    const uint32_t articles = a->hasArticleList() ? a->articleListCount() : a->titleCount();
    about = pocketlib::formatCount(articles) + " titles";
    if (collection_ < cols.size()) {
      if (!cols[collection_].date.empty()) about += " \xC2\xB7 " + cols[collection_].date;
      about += " \xC2\xB7 " + pocketlib::formatBytes(cols[collection_].bytes);
    }
    about += lib.titleIndex(collection_) ? "" : " \xC2\xB7 no search index";
  }
  const pocketlib::OpenTimings& t = lib.lastOpen;
  if (t.valid) {
    char buf[96];
    snprintf(buf, sizeof(buf), "\nLast article: read %u ms%s, first page %u ms", t.readMs,
             t.clusterCached ? " (cached)" : "", t.firstPageMs);
    about += buf;
  }
  add(RowKind::About, 0, "About this collection", about);

  items_.assign(rows_.size() - 1, fui::ListItem{});
  for (size_t i = 0; i < items_.size(); i++) {
    items_[i].label = labels_[i].c_str();
    items_[i].subtitle = subtitles_[i].empty() ? nullptr : subtitles_[i].c_str();
    items_[i].actionValue = static_cast<int16_t>(i + 1);
    const RowKind kind = rows_[i + 1].kind;
    items_[i].enabled = kind == RowKind::About || kind == RowKind::Place || a != nullptr;
  }
}

void CollectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), side,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), side});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing + 4));

  // The search bar: the collection's icon and "Search Wikipedia" in a
  // rounded field; a heavier border while the buttons are on it.
  constexpr int16_t kBarHeight = 58;
  const fui::Rect bar = screen.takeTop(kBarHeight, 14);
  fui::ButtonProps props;
  props.label = searchLabel_.c_str();
  if (icon_) props.icon = fui::bitmapFromIcon(*icon_);
  props.iconSize = 28;
  props.gap = 12;
  props.action = ACTION_SEARCH_BAR;
  props.inputMask = fui::InputTouch;
  fui::StyleSet styles = fui::tileGridStyles(kBarHeight / 2);
  fui::BoxStyle focus = styles.normal;
  focus.borderWidth = 4;
  styles.selected = styles.focused = styles.active = focus;
  props.styles = styles;
  props.state = nav.selected == 0 ? fui::StateChecked : fui::StateNormal;
  screen.button(props, bar);

  fui::ListProps list;
  list.inputMask = fui::InputTouch;
  list.subtitleText = screen.theme().smallText;
  list.subtitleText.maxLines = 2;
  list.items = items_.data();
  list.count = static_cast<uint16_t>(items_.size());
  list.action = ACTION_ROW;
  syncListViewport(screen, list, 1);
  screen.list(list);
}

void CollectionActivity::activateIndex(int index) {
  notice_.clear();
  if (index < 0 || index >= static_cast<int>(rows_.size())) return;
  const Row row = rows_[index];
  switch (row.kind) {
    case RowKind::Search:
      openSearch();
      return;
    case RowKind::Place:
      openPlace(row.place);
      return;
    case RowKind::Main:
      openMain();
      return;
    case RowKind::Random:
      openRandom();
      return;
    case RowKind::About:
      return;
  }
}

void CollectionActivity::openPlace(size_t place) {
  const auto& places = pocketlib::ReadingHistory::instance().places();
  size_t collection = 0;
  uint32_t entry = 0;
  if (place < places.size() && locatePlace(places[place], collection, entry) && collection == collection_) {
    openEntry(entry);
  } else {
    showMessage("That article is no longer on the card");
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
  notice_ = message;
  requestUpdate();
}

void CollectionActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (!notice_.empty()) GUI.drawPopup(renderer, notice_.c_str());
}

// ------------------------------------------------------------ choice list

ChoiceListActivity::ChoiceListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                       std::vector<std::string> labels, std::vector<std::string> subtitles, int initial)
    : UiListActivity("PocketChoice", renderer, mappedInput),
      title_(std::move(title)),
      labels_(std::move(labels)),
      subtitles_(std::move(subtitles)),
      initial_(initial) {}

void ChoiceListActivity::onEnter() {
  items_.assign(labels_.size(), fui::ListItem{});
  for (size_t i = 0; i < items_.size(); i++) {
    items_[i].label = labels_[i].c_str();
    items_[i].subtitle = i < subtitles_.size() && !subtitles_[i].empty() ? subtitles_[i].c_str() : nullptr;
    items_[i].actionValue = static_cast<int16_t>(i);
  }
  UiListActivity::onEnter();
  if (initial_ > 0 && initial_ < static_cast<int>(items_.size())) moveSelectionTo(initial_);
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
