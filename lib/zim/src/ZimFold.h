// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Search-key folding: makes "forbidden city", "Forbidden City" and
// "FÖRBIDDEN  CITY" compare equal. The card builder folds every title with
// this function and the device folds what the reader types with the same
// function, so the two always agree.
//
// Rules: ASCII letters lower-cased; code points in the generated table
// (scripts/gen_fold_table.py: accents dropped, case folded, ligatures and
// typographic punctuation mapped to ASCII) replaced; runs of whitespace
// collapsed to one space; leading and trailing whitespace dropped.
// Everything else, including CJK, passes through unchanged. Invalid UTF-8
// bytes pass through as they are.

#include <cstdint>
#include <string>
#include <string_view>

namespace zim {

// Bump when the folding rules or table change: every title index built with
// an older version must then be rebuilt.
constexpr uint32_t kFoldVersion = 1;

// Longest folded key kept, in bytes; longer keys are cut on a UTF-8 boundary.
constexpr size_t kMaxKeyBytes = 255;

std::string foldKey(std::string_view text);

}  // namespace zim
