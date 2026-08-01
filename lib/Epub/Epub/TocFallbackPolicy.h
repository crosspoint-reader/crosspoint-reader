#pragma once

// Pure decision predicates for the nav-vs-NCX TOC fallback in Epub's TOC pass.
// Kept dependency-free so the adoption logic is unit-testable on the host.
// "Mapped" counts are TOC entries whose href resolved to a spine item
// (TocEntry::spineIndex >= 0); unmapped entries display but cannot navigate,
// so a stale NCX full of dead links must not supersede a smaller mapped nav.
namespace toc_fallback {

// A parsed nav is sparse when its mapped entries cover < half the spine.
constexpr bool navIsSparse(const bool navParsed, const int navMappedCount, const int spineItemCount) {
  return navParsed && navMappedCount * 2 < spineItemCount;
}

// NCX wins only when parsed and strictly richer in mapped entries (tie keeps
// nav, the EPUB 3 preferred source). If nav didn't parse, any parsed NCX wins.
constexpr bool shouldAdoptNcx(const bool navParsed, const int navMappedCount, const bool ncxParsed,
                              const int ncxMappedCount) {
  return ncxParsed && (!navParsed || ncxMappedCount > navMappedCount);
}

}  // namespace toc_fallback
