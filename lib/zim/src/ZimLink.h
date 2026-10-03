// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Following a link inside an archive. Kiwix pages link to each other with
// relative URLs ("Climate_system", "../C/Beijing#History", "Earth%27s_orbit"),
// relative to the linking entry's path; the old namespace scheme climbs out of
// the namespace ("../A/Foo"). This turns such an href into an entry and an
// optional #fragment, without touching the network or the page's base URL.

#include <string>
#include <string_view>

#include "ZimArchive.h"

namespace zim {

struct LinkTarget {
  char ns = 0;
  std::string path;       // decoded, resolved against the linking entry
  std::string fragment;   // decoded, without '#'; may be empty
  bool samePage = false;  // only a fragment ("#Notes")
};

// True for hrefs that leave the archive (http:, mailto:, //host, ...).
bool isExternalHref(std::string_view href);

// Resolves `href` as found on the page of `from`. False for external or
// empty hrefs.
bool parseLink(const Entry& from, std::string_view href, LinkTarget& out);

// parseLink + directory lookup + redirects. `out` is the content entry.
// Returns NotFound when the target is not in this archive.
Error resolveLink(Archive& archive, const Entry& from, std::string_view href, Entry& out, std::string& fragment);

}  // namespace zim
