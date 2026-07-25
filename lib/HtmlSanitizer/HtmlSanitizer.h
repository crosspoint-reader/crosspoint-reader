#pragma once

#include <string>

// Generic HTML5-to-XML sanitizer: turns loosely-formed HTML into the strict, well-formed
// XML that expat requires. Not specific to any particular parser or document type.
namespace HtmlSanitizer {

// Rewrites src into dst, self-closing HTML5 void elements (<br> -> <br/>) and dropping
// void-element end tags (<br></br> -> <br/>) so expat's strict XML parser accepts the
// result. Returns false on any I/O error (dst is not usable; caller should fall back to
// src). On success, `modified` reports whether dst differs from src, so the caller can
// skip swapping files when the source was already well-formed.
bool selfCloseVoidElements(const std::string& src, const std::string& dst, bool& modified);

}  // namespace HtmlSanitizer
