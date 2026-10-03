// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Turns an article's HTML, as Kiwix stores it, into small well-formed XHTML
// that CrossPoint's EPUB layout engine (expat + ChapterHtmlSlimParser) can lay
// out. Kiwix pages are HTML5 written for browsers: unclosed <meta>/<p>/<li>,
// named entities expat does not know, scripts, styles, image boxes and
// navigation templates. The cleaner
//  * keeps the reading structure: headings, paragraphs, lists, block quotes,
//    tables, bold/italic/underline/strike, sub/superscript, line breaks;
//  * drops what an offline e-ink reader cannot use (scripts, styles, media,
//    forms, edit links, navboxes, citation markers, hidden elements);
//  * unwraps everything else, keeping its text;
//  * decodes entities to UTF-8, repairs bad UTF-8, removes characters XML
//    forbids, and closes every element it opens.
// It streams: memory use is the open-element stack plus a small output buffer,
// whatever the article's size. No device dependencies, so it is host-tested.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace zim {

class HtmlSink {
 public:
  virtual ~HtmlSink() = default;
  // Returns false to abort (e.g. the card is full).
  virtual bool write(const char* data, size_t len) = 0;
};

class StringHtmlSink final : public HtmlSink {
 public:
  bool write(const char* data, size_t len) override {
    out.append(data, len);
    return true;
  }
  std::string out;
};

struct HtmlCleanOptions {
  // Written into <title>; the article's own <h1> stays in the body.
  std::string_view title;
  // Keep <a href> for links inside the archive. Off until the reader can
  // follow links (Milestone 5): otherwise every linked word is underlined.
  bool keepLinks = false;
};

struct HtmlCleanStats {
  size_t inputBytes = 0;
  size_t outputBytes = 0;
  uint32_t elementsKept = 0;
  uint32_t subtreesDropped = 0;
};

// Returns false only if the sink refused a write. Malformed input never fails.
bool cleanArticleHtml(std::string_view html, const HtmlCleanOptions& options, HtmlSink& sink,
                      HtmlCleanStats* stats = nullptr);

// Decodes one entity body (without '&' and ';'), e.g. "amp", "#233", "#x1F600".
// Returns the code point, or 0 if unknown.
uint32_t decodeHtmlEntity(std::string_view name);

}  // namespace zim
