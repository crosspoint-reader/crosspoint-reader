#pragma once

// MOBI -> EPUB transmuter. Converts a DRM-free MOBI6 book into a minimal
// EPUB2 on the SD card, then the stock EPUB pipeline (Epub/Section/
// ChapterHtmlSlimParser) opens it natively — TOC, page cache, progress and
// fonts all work unchanged. Conversion runs once per book and is cached
// under /.crosspoint/mobi_<hash>/book.epub keyed on source size+textLength.
//
// The tidy pass rewrites MOBI's HTML-3.2-ish markup into well-formed XHTML
// (expat downstream is strict): tag whitelist with auto-close, void-element
// self-closing, attribute quoting, named-entity -> numeric mapping, cp1252 ->
// UTF-8 transcoding, <mbp:pagebreak> chapter splits, and recindex image
// extraction. Text is never dropped — unknown tags are stripped, their
// content kept.

#include <string>

class MobiToEpub {
 public:
  // Returns the converted EPUB path, or "" on failure (errorOut gets a
  // user-showable reason). Reuses the cached conversion when valid.
  static std::string ensureConverted(const std::string& mobiPath, std::string* errorOut = nullptr);
};
