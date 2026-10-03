// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimLink.h"

#include <vector>

namespace zim {
namespace {

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexValue(s[i + 1]);
      const int lo = hexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// "&amp;" survives in hrefs that were copied out of HTML attribute text.
std::string unescapeAmp(std::string_view s) {
  std::string out(s);
  for (size_t p = out.find("&amp;"); p != std::string::npos; p = out.find("&amp;", p + 1)) out.erase(p + 1, 4);
  return out;
}

}  // namespace

bool isExternalHref(std::string_view href) {
  auto starts = [&](std::string_view p) { return href.substr(0, p.size()) == p; };
  if (starts("//")) return true;
  // A scheme: letters, digits, + - . before the first ':' and no '/' before it.
  const size_t colon = href.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  for (size_t i = 0; i < colon; i++) {
    const char c = href[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!ok) return false;
  }
  return true;
}

bool parseLink(const Entry& from, std::string_view rawHref, LinkTarget& out) {
  out = LinkTarget{};
  const std::string href = unescapeAmp(rawHref);
  std::string_view h = href;
  if (h.empty() || isExternalHref(h)) return false;

  const size_t hash = h.find('#');
  std::string_view pathPart = h.substr(0, hash);
  if (hash != std::string_view::npos) out.fragment = percentDecode(h.substr(hash + 1));
  const size_t query = pathPart.find('?');
  if (query != std::string_view::npos) pathPart = pathPart.substr(0, query);

  out.ns = from.ns;
  if (pathPart.empty()) {
    out.samePage = true;
    out.path = from.path;
    return !out.fragment.empty();
  }

  // Start from the linking entry's directory, unless the href is absolute.
  std::vector<std::string> parts;
  bool absolute = false;
  std::string_view rest = pathPart;
  if (rest.front() == '/') {
    absolute = true;
    while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
  } else {
    std::string_view base = from.path;
    const size_t slash = base.rfind('/');
    base = slash == std::string_view::npos ? std::string_view() : base.substr(0, slash);
    size_t start = 0;
    while (start < base.size()) {
      size_t end = base.find('/', start);
      if (end == std::string_view::npos) end = base.size();
      if (end > start) parts.emplace_back(base.substr(start, end - start));
      start = end + 1;
    }
  }

  bool escaped = absolute;  // climbed above the namespace: the next segment names one
  size_t start = 0;
  while (start <= rest.size()) {
    size_t end = rest.find('/', start);
    if (end == std::string_view::npos) end = rest.size();
    const std::string seg = percentDecode(rest.substr(start, end - start));
    start = end + 1;
    if (seg.empty() || seg == ".") {
      if (end == rest.size()) break;
      continue;
    }
    if (seg == "..") {
      if (parts.empty())
        escaped = true;
      else
        parts.pop_back();
    } else if (escaped && parts.empty() && seg.size() == 1) {
      out.ns = seg[0];  // "../A/Foo", "/C/Foo"
      escaped = false;
    } else {
      parts.push_back(seg);
      escaped = false;
    }
    if (end == rest.size()) break;
  }
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out.path.push_back('/');
    out.path += parts[i];
  }
  return !out.path.empty();
}

Error resolveLink(Archive& archive, const Entry& from, std::string_view href, Entry& out, std::string& fragment) {
  LinkTarget t;
  if (!parseLink(from, href, t)) return Error::NotFound;
  fragment = t.fragment;
  Error err = archive.findByPath(t.ns, t.path, out);
  if (err != Error::None) return err;
  return archive.resolve(out);
}

}  // namespace zim
