// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

namespace pocketlib {
struct Place;
struct Collection;
}  // namespace pocketlib
namespace freeink {
struct Icon;
}

// The collection and directory entry of a saved place; false if its
// collection or article is no longer on the card.
bool locatePlace(const pocketlib::Place& place, size_t& collection, uint32_t& entry);

// Home -> Library: a grid of tiles, two across. The top level is Recent,
// eBooks (CrossPoint's own book library), then one tile per group of
// collections: Wikipedia, Maps (not yet), Medical and More. A group with one
// collection opens it; with several, it opens the same grid for its members.
// Groups come from the manifest's "group" field, else from the collection's
// name (tileGroupFor).
class ShelfActivity final : public UiListActivity {
 public:
  ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string group = "");
  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(tiles_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void navigateButtons() override;
  void drawFooter() override;
  const char* headerTitle() const override { return title_.c_str(); }

  enum class TileKind : uint8_t { Recent, Books, Group, Collection, Maps };
  struct Tile {
    TileKind kind;
    std::string label;
    const freeink::Icon* icon;
    int collection;     // TileKind::Collection
    std::string group;  // TileKind::Group
  };
  void rebuildTiles();
  void openRecent();
  void openCollection(size_t collection);

  const std::string group_;  // "" = the top level
  std::string title_;
  std::vector<Tile> tiles_;
  std::vector<freeink::ui::TileGridItem> items_;
  bool buttonsUsed_ = false;  // show the selection only once buttons move it
  // Grid geometry from the last build: drawFooter() draws each tile's icon
  // and name itself (icon above, name below, cut to the tile's width).
  freeink::ui::Rect gridRect_{};
  int16_t tileHeight_ = 0;
  std::string notice_;  // a one-line popup over the grid, cleared on the next input
};

// "Wikipedia", "Medical" or "More": the manifest's group, else by name.
std::string tileGroupFor(const pocketlib::Collection& collection);
// A collection's name short enough for a tile: "MedlinePlus", not
// "MedlinePlus - Health Information from the National Library of Medicine".
std::string shortTitle(const pocketlib::Collection& collection);

// A titled list of choices; finishes with MenuResult::action = the row picked
// (cancelled on Back). Used for an article's contents and the Recent list.
class ChoiceListActivity final : public UiListActivity {
 public:
  ChoiceListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                     std::vector<std::string> labels, std::vector<std::string> subtitles = {});
  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(items_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return title_.c_str(); }

  std::string title_;
  std::vector<std::string> labels_;
  std::vector<std::string> subtitles_;
  std::vector<freeink::ui::ListItem> items_;
};

// One collection's home, like the Wikipedia app's: a search bar with the
// collection's icon first, then the article to continue, recent articles,
// the main page, a random article, and About (size, index, last timings).
class CollectionActivity final : public UiListActivity {
 public:
  CollectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection);
  void onEnter() override;

 private:
  static constexpr freeink::ui::ActionId ACTION_SEARCH_BAR = ACTION_USER;
  enum class RowKind : uint8_t { Search, Place, Main, Random, About };
  struct Row {
    RowKind kind;
    size_t place;  // RowKind::Place: index into ReadingHistory::places()
  };

  int listCount() const override { return static_cast<int>(rows_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }

  void refreshRows();
  void openEntry(uint32_t entryIndex);
  void openRandom();
  void openMain();
  void openSearch();
  void openPlace(size_t place);
  void showMessage(const std::string& message);
  void drawFooter() override;

  const size_t collection_;
  std::string title_;
  std::string searchLabel_;
  std::string notice_;  // a popup over the screen, cleared by the next choice
  const freeink::Icon* icon_ = nullptr;
  std::vector<Row> rows_;  // rows_[0] is the search bar
  std::vector<std::string> labels_;
  std::vector<std::string> subtitles_;
  std::vector<freeink::ui::ListItem> items_;  // rows_[1..]
};
