#pragma once
// Drives the REAL EPUB compilation pipeline (Epub load/index, CSS compile,
// footnote gather, per-spine Section build) on the host and emits a canonical
// text dump of the resulting layout: per page, every element's position, every
// word's text/x-position/style. Two runs over the same book with the same
// profile must produce byte-identical dumps (determinism), and any layout
// refactor must keep the dump unchanged (golden equivalence).
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

class BuildArena;
struct BlockFontFamilies;

namespace pipeline_harness {

// A named render-settings profile — the section-cache variant under test.
struct Profile {
  const char* name = "default";
  int fontId = 1;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 460;
  uint16_t viewportHeight = 760;
  bool hyphenationEnabled = false;
  bool fontSizeNormalization = true;
  bool embeddedStyle = true;
  bool bionicReadingEnabled = false;
  bool inlineFootnotePreviews = true;
  uint8_t imageRendering = 0;
  // The book's other font families (Publisher Fonts' Accents and All Text); nullptr for none.
  const BlockFontFamilies* fontFamilies = nullptr;
  // When non-zero, every section build is lent a heap-backed BuildArena of this many bytes, the
  // way the reader lends the borrowed secondary framebuffer to a background build. The heap
  // census then shows what such a build keeps on the heap; the arena's own use is reported per
  // spine (ARENA lines) when `arenaStat` is set.
  size_t lentArenaBytes = 0;
};

using ArenaStatFn = std::function<void(int spineIndex, size_t highWater, size_t capacity)>;

// Called around each spine's section build (createSectionFile only, not the harness read-back):
// begin=true right before it, false right after. `lentArena` is the region lent to the build, or
// nullptr in heap-only mode. The epub_build_inventory tool opens its measurement window here.
using BuildBracketFn = std::function<void(int spineIndex, bool begin, ::BuildArena* lentArena)>;
void setBuildBracket(BuildBracketFn fn);

// Called after each spine item's build+dump with its wall-clock cost.
// pages/elapsedUs cover the section build AND the page-by-page dump read-back.
using SpineStatFn = std::function<void(int spineIndex, uint16_t pages, int64_t elapsedUs)>;

// Compile `epubPath` into `cacheDir` under `profile` and stream the canonical
// dump to `out`. Returns false on any pipeline failure (already-logged).
// `cacheDir` should be empty/fresh for a cold run. A second call over the same
// cacheDir still rebuilds every section, but from the book-level caches the first
// left behind (book.bin, CSS index, image manifest, banked XHTML, footnote store).
bool runAndDump(const std::string& epubPath, const std::string& cacheDir, const Profile& profile, std::ostream& out,
                const SpineStatFn& spineStat = {}, const ArenaStatFn& arenaStat = {});

}  // namespace pipeline_harness
