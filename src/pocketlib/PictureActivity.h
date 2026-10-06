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

#include "activities/Activity.h"

// A picture from an article on its own, as large as the screen allows, in
// grey. Opened by tapping the picture; any tap, button or Back returns to the
// page. The picture is the file the page already extracted (a PNG or JPEG
// under /.pocketlib/img), decoded once more at this size.
class PictureActivity final : public Activity {
 public:
  PictureActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string imagePath)
      : Activity("PocketPicture", renderer, mappedInput), imagePath_(std::move(imagePath)) {}
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string imagePath_;
};
