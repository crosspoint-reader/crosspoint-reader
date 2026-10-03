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

// Home -> Library: Recent (articles left part-read), the collections on the
// card, then CrossPoint's own book library as the last row.
class ShelfActivity final : public UiListActivity {
 public:
  ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return "Library"; }

  enum class RowKind : uint8_t { Recent, Collection, Books, Empty };
  struct Row {
    RowKind kind;
    int collection;
  };
  void rebuildRows();
  void openRecent();

  std::vector<Row> rows_;
  std::vector<std::string> labels_;
  std::vector<std::string> subtitles_;
  std::vector<std::string> values_;
  std::vector<freeink::ui::ListItem> items_;
};

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

// One collection: ways into it. Search (live, as you type), the main page and
// a random article.
class CollectionActivity final : public UiListActivity {
 public:
  CollectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection);
  void onEnter() override;

 private:
  enum Row { ROW_SEARCH = 0, ROW_MAIN, ROW_RANDOM, ROW_ABOUT, ROW_LAST_OPEN, ROW_COUNT };

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }

  void refreshRows();
  void openEntry(uint32_t entryIndex);
  void openRandom();
  void openMain();
  void openSearch();
  void showMessage(const std::string& message);

  const size_t collection_;
  std::string title_;
  std::string values_[ROW_COUNT];
  std::string subtitles_[ROW_COUNT];
  freeink::ui::ListItem items_[ROW_COUNT]{};
  std::string lastQuery_;
  std::vector<uint32_t> liveEntries_;  // entries behind the keyboard's suggestion rows
};
