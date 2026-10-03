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
// collections already on the card. First Aid rows name a MedlinePlus page by
// its permanent address (opened at its "First Aid" section), else a chapter
// of the Wikibooks First Aid manual. Encyclopedia rows name titles in order of
// preference (MedlinePlus, then Wikipedia), looked up exactly through the
// card's search index. A row nothing on the card answers is left out.
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
    std::string landing;  // section to open at ("First Aid"); "" = the usual place
  };

  int listCount() const override { return static_cast<int>(rows_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title_.c_str(); }
  void resolve();
  void resolveFirstAid(int medline, int wikibooks);
  void resolveTopics(const int* collectionOf);  // indexed by source

  const Kind kind_;
  std::string title_;
  std::vector<Row> rows_;
  std::vector<freeink::ui::ListItem> items_;
};
