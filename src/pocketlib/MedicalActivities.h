// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Medical → First Aid and Medical Encyclopedia: hand-made lists over the
// collections already on the card. Each row names the article it wants by
// title, in order of preference ("Choking - adult or child over 1 year" in
// MedlinePlus, else "Choking" in MDWiki or Wikipedia); the titles are looked
// up (exact, through the card's search index) when the screen opens, and a
// row nothing on the card answers is left out.
class CuratedListActivity final : public UiListActivity {
 public:
  enum class Kind : uint8_t { FirstAid, Encyclopedia };
  CuratedListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Kind kind);
  void onEnter() override;

 private:
  struct Row {
    std::string label;
    std::string subtitle;
    int collection = -1;  // -1: the search row (Encyclopedia)
    uint32_t entry = 0;
  };

  int listCount() const override { return static_cast<int>(rows_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }
  void resolve();

  const Kind kind_;
  std::string title_;
  std::vector<Row> rows_;
  std::vector<freeink::ui::ListItem> items_;
};
