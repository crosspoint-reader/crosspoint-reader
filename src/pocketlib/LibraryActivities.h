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

// Home -> Library: the collections on the card, then CrossPoint's own book
// library as the last row.
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

  std::vector<std::string> subtitles_;
  std::vector<std::string> values_;
  std::vector<freeink::ui::ListItem> items_;
};

// One collection: ways into it. Milestone 3 has the main page, a random
// article and an exact title; search arrives in Milestone 4.
class CollectionActivity final : public UiListActivity {
 public:
  CollectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t collection);
  void onEnter() override;

 private:
  enum Row { ROW_MAIN = 0, ROW_RANDOM, ROW_TITLE, ROW_ABOUT, ROW_LAST_OPEN, ROW_COUNT };

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }

  void refreshRows();
  void openEntry(uint32_t entryIndex);
  void openRandom();
  void openMain();
  void askTitle();
  void openTitle(const std::string& query);
  void showMessage(const std::string& message);

  const size_t collection_;
  std::string title_;
  std::string values_[ROW_COUNT];
  std::string subtitles_[ROW_COUNT];
  freeink::ui::ListItem items_[ROW_COUNT]{};
  std::string lastQuery_;
};
