#pragma once

#include <cstdint>
#include <string_view>

// How a book-relative href (decoded, normalised) matches one spine item's href: the same path, or
// only the same filename (last path segment). In-book links are kept as written, relative to their
// own document, and TOC documents can link relative to a folder they do not sit in (nav.xhtml in
// OEBPS/ linking "c1.xhtml" for OEBPS/text/c1.xhtml), so a filename match is what resolves them.
// Shared by Epub::resolveHrefToSpineIndex and BookMetadataCache's TOC lookup; callers walk the
// spine where it lives (book.bin, or the spine temp file during a build), never a RAM table.
enum class SpineHrefMatch : uint8_t { None, Filename, Exact };

namespace spine_href {

inline std::string_view filenameOf(const std::string_view path) {
  const size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

inline SpineHrefMatch match(const std::string_view href, const std::string_view spineHref) {
  if (spineHref == href) return SpineHrefMatch::Exact;
  const std::string_view name = filenameOf(href);
  if (!name.empty() && filenameOf(spineHref) == name) return SpineHrefMatch::Filename;
  return SpineHrefMatch::None;
}

}  // namespace spine_href
