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
#include <vector>

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

// A heading found while cleaning, for an article's table of contents. Its
// element is written with id="pl-h<N>" (N = its position in the list), so the
// layout engine records which page it lands on.
struct HtmlHeading {
  uint8_t level = 2;                 // 1..6
  std::string text;                  // plain UTF-8, whitespace collapsed
  std::vector<std::string> aliases;  // the page's own ids on or inside it ("History"), for #fragment links
  // The section's opening sentence (its first paragraph or list item before
  // the next heading), for the contents list; empty when it opens straight
  // into a subsection or a table.
  std::string summary;
};

// "pl-h3"
std::string headingAnchor(size_t index);

// An image the cleaner kept: written as <img src="/pl-img/<n>.png"
// width=.. height=..> (<n>.jpg for a JPEG), where n is its position in
// HtmlCleanOptions::imageList. The reader makes that file from `src` (the
// article's own reference to the picture in the archive) when the image's
// page is first drawn: a WebP is converted, a JPEG or PNG copied.
enum class ImageFormat : uint8_t { Unknown, WebP, Png, Jpeg };
struct HtmlImage {
  std::string src;
  int width = 0;
  int height = 0;
  ImageFormat format = ImageFormat::Unknown;  // from the src's extension
};
enum class HtmlImages : uint8_t {
  None,  // drop every image (and its frame and caption)
  Lead,  // the first picture before the first section (an infobox photo)
  All,
};
constexpr const char* kArticleImagePrefix = "/pl-img/";

struct HtmlCleanOptions {
  // Written into <title>; the article's own <h1> stays in the body.
  std::string_view title;
  // Keep <a href> for links inside the archive. Off until the reader can
  // follow links (Milestone 5): otherwise every linked word is underlined.
  bool keepLinks = false;
  // When set, headings are collected here and get the "pl-h<N>" ids above
  // (instead of the page's own ids).
  std::vector<HtmlHeading>* headings = nullptr;
  // When set, the article's opening paragraphs as plain text (before its
  // first section heading, outside tables and lists), at most leadLimit
  // bytes, cut at a word: for outlines and link previews.
  std::string* lead = nullptr;
  size_t leadLimit = 600;
  // Images to keep, collected in imageList (required unless None). Only
  // WebP pictures at least 60x40: icons, flags and formula images stay out.
  HtmlImages images = HtmlImages::None;
  std::vector<HtmlImage>* imageList = nullptr;
};

// Up to `maxSentences` sentences of `text` (cut at ". ", "! " or "? "
// followed by a capital), at most `maxBytes`, ending with "…" if cut short.
std::string firstSentences(std::string_view text, size_t maxSentences, size_t maxBytes);

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
