// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Hidden diagnostics screen (Settings -> About, tap "Firmware" five times).
// Reports the hardware numbers the library's performance budget depends on:
// PSRAM and internal heap, flash, the SD bus as the driver actually clocked it,
// and an on-demand SD benchmark (sequential throughput and random 4 KB reads).
// Every value is also logged over serial with the "DIAG" tag.
class DiagnosticsActivity final : public UiListActivity {
 public:
  explicit DiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 11;

  void onEnter() override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override { return "Diagnostics"; }

  void refreshMemoryRows();
  void runBenchmark();

  bool benchmarkPending_ = false;
  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
