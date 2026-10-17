#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HeapFit.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxParser.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include "../../Epub.h"
#include "../HashUtils.h"
#include "../Page.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"

namespace {
// An entry whose name has no extension at all: FB2-to-EPUB conversions write <img
// src="images/img_32"/>. Only its bytes can say whether it is an image we decode.
bool hasNoFileExtension(const std::string& resolvedPath) {
  const size_t slash = resolvedPath.rfind('/');
  return resolvedPath.find('.', slash == std::string::npos ? 0 : slash + 1) == std::string::npos;
}

// Cache filename for an image extracted out of the EPUB, keyed by the archive entry it came from.
//
// The entry path is the only thing that identifies these bytes. It is deliberately NOT the parse
// order: imageCounter used to serve that role, but buildCellImage() bails before incrementing it
// when an image's dimensions cannot be resolved, and dimension resolution depends on images.bin
// filling in over time -- so the same document could number its images differently between runs
// and hand a block the file belonging to a different image. Hashing the entry cannot drift.
//
// The extension is the entry name's, or .img when the name has none. The decoder goes by the bytes
// (ImageBlock::render), but the pixel-cache names are cut at the last dot: with none in the file
// name the cut would land on the one in /.crosspoint, and every such image would share one .pxc.
std::string imageCachePathFor(const std::string& imageBasePath, const std::string& resolvedPath) {
  std::string ext = ".img";
  if (!hasNoFileExtension(resolvedPath)) ext = resolvedPath.substr(resolvedPath.rfind('.'));
  char hash[17];
  snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(HashUtils::fnvHash64(resolvedPath)));
  return imageBasePath + hash + ext;
}

// Whether an image reference is worth resolving. A known extension goes by its name, as before; a
// name without one is let through and judged by its bytes when its dimensions are read (the image
// manifest and the ZIP header read both sniff the format, and neither answers for an unknown one),
// so a non-image still ends as alt text. A name with some other extension (.svg, .webp) is still
// turned away by name, without a ZIP read.
// Based on crosspoint-reader PR #2386 (artwen, @artwem).
bool isImageCandidate(const std::string& resolvedPath) {
  return hasNoFileExtension(resolvedPath) || ImageDecoderFactory::isFormatSupported(resolvedPath);
}
}  // namespace

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Size thresholds (bytes of XHTML) controlling indexing popup behavior.
// Each progress callback costs ~640ms of e-ink refresh, so we trade granularity off
// against indexing time based on expected duration.
//   < 15KB:  no popup at all - indexing finishes faster than the popup would draw
//   < 30KB:  popup only (one refresh up-front, no mid-parse updates)
//   < 80KB:  popup + one heartbeat at 50%
//   >= 80KB: popup + ticks at 25/50/75%
constexpr size_t MIN_SIZE_FOR_POPUP = 15 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_HEARTBEAT = 30 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_FINE = 80 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_INDEXING_POPUP = 32 * 1024;
constexpr size_t MIN_CONTIG_HEAP_FOR_INDEXING_POPUP = 12 * 1024;

constexpr size_t PARSE_BUFFER_SIZE = 1024;

// Hard cap on the number of anchor IDs recorded per chapter. Legitimate navigation
// anchors (TOC entries, footnotes, cross-references) rarely exceed a few hundred per
// chapter. A runaway count usually means a converter injected machine-generated IDs on
// every text fragment (e.g. Kobo KePub spans). The cap prevents unbounded heap growth
// on resource-constrained devices. TOC anchors bypass this cap.
// What bounds this is no longer the heap. Anchors stream to SD (see setAnchorSpillPath), so a
// chapter with ten thousand of them would cost the same RAM as one with ten. Two costs remain,
// and both are why the cap is still here rather than raised to the uint16_t the format allows:
// every anchor adds ~14 bytes to the section cache, and getPageForAnchor is a LINEAR scan that
// reads entries one at a time off the card, so the cap is also the worst-case cost of a single
// footnote jump. Raising it trades navigation latency for reach.
//
// 1024 covers the books measured: the heaviest real chapters seen here are an endnotes document
// at 545 and an index chapter at 675, and converters that split notes across many small files
// keep per-chapter counts lower still. Raise it when a real book is found that needs it -- and
// give getPageForAnchor an index first if the number goes far past this.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;
// The bound when anchors could NOT be spilled and are being held in RAM instead. Much lower than
// the spilled cap on purpose: the resident path exists only for a chapter whose spill file would
// not open, which means the SD is already unhappy -- and holding 1024 std::pairs (28.7 KB, the
// exact allocation the spill was introduced to remove, on a plain vector whose push_back aborts
// under -fno-exceptions) is the worst possible response to that. 256 keeps a chapter's early
// anchors navigable for ~7 KB; the rest degrade the same way they do past any cap.
constexpr size_t MAX_RESIDENT_ANCHORS = 256;
// Ids on <span> recorded per chapter, on top of MAX_ANCHORS_PER_CHAPTER (recordSpanAnchor). Some
// books put their link targets on spans -- FB2 conversions wrap every note in one -- and a notes
// chapter measured 151 of them; converters that tag every text run with a span id would otherwise
// add thousands of entries no link names. Half the general cap: room for any notes chapter seen,
// and a bound on what a converter-tagged chapter adds to the map and to a footnote jump's scan.
constexpr size_t MAX_SPAN_ANCHORS_PER_CHAPTER = 512;
// Write buffer for the anchor spill; see the emplace site in setup().
constexpr size_t ANCHOR_SPILL_BUFFER_BYTES = 512;
// Anchors that may wait for their first line at once. One per id'd element that has produced no
// text yet; more than a couple means a run of empty anchored elements, and the excess is recorded
// at the current page rather than held.
constexpr size_t MAX_ANCHORS_AWAITING_LINE = 16;

// Image extraction is now deferred to render time (ImageBlock::ensureExtracted).
// No heap guard needed at parse time — only a ZIP header read (~4 KB buffer on stack in
// getDimensionsFromZipEntry) happens during createSectionFile.

#ifndef EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP
#define EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP (18 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC
#define EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC (12 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP
#define EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP (9 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC
#define EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC (6 * 1024)
#endif

// The hard contiguous floor for a build whose line bytes live in the build arena (setBuildArena):
// there the largest heap request phase (b) still makes is the word vector of a 97-word block
// (the long-block split bound) at its 128-entry capacity, 128 * sizeof(std::string) = 3,072 B,
// and everything else per paragraph is smaller. The 6 KB floor above was sized when every
// line's TextBlock bytes came from the heap too; keeping it for arena builds aborted Strange
// Pictures' Chapter 3 at page 146 with 3,956 B contiguous and 14 KB free (device run 10), and
// the released rebuild that followed is what fragments the heap for the rest of the session.
// Bionic reading doubles the word count of the layout scratch and builds a transformed copy of
// the block's tail (up to 2 * 97 entries), so it keeps a higher floor.
#ifndef EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA
#define EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA (3584)
#endif
#ifndef EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA_BIONIC
#define EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA_BIONIC (5 * 1024)
#endif

// Reading an image header straight out of the ZIP
// (ImageDecoderFactory::getDimensionsFromZipEntry) — the only allocation in image handling big
// enough to be worth gating, see the image branch in startElement.
//
// 40/34 KB -> 16/8 KB. The old numbers were sized for a ~32 KB inflate ring that this path no
// longer takes: ZipFile::readBytesFromStat now sizes the ring to the bytes actually wanted
// (see the comment there, and its own device measurement), which for a 4 KB header read is a
// 4 KB ring. The gate was never retuned with it, so it went on demanding three times what the
// operation costs.
//
// That was not merely conservative. A refusal drops the image to alt text, and the section is
// then CACHED that way under an unchanged property hash — nothing ever rebuilds it. Background-B
// builds with the framebuffer borrowed, which is exactly when the largest free block is small,
// so on X4 the old gate refused at 50168 free / 30708 contig and baked a missing image into a
// chapter permanently. What the read actually needs: a 4 KB header buffer, a 512 B read buffer,
// a ring of up to 4 KB, and (unprimed) a 4 KB EOCD scan window — ~13 KB total, largest single
// block 4 KB. 16/8 covers that with room to spare and is reachable from a borrowed-buffer build.
#ifndef EHP_IMAGE_HEADER_MIN_FREE_HEAP
#define EHP_IMAGE_HEADER_MIN_FREE_HEAP (16 * 1024)
#endif
#ifndef EHP_IMAGE_HEADER_MIN_MAX_ALLOC
#define EHP_IMAGE_HEADER_MIN_MAX_ALLOC (8 * 1024)
#endif
constexpr size_t MIN_FREE_HEAP_FOR_IMAGE_HEADER = EHP_IMAGE_HEADER_MIN_FREE_HEAP;
constexpr size_t MIN_MAX_ALLOC_FOR_IMAGE_HEADER = EHP_IMAGE_HEADER_MIN_MAX_ALLOC;

// Largest-free-block readings land a few bytes UNDER the round number -- the allocator's own
// bookkeeping, documented at ensureHeapForTextLayout below. Any contiguous threshold that DROPS
// CONTENT when it is missed subtracts this, so a block genuinely of the required size is never
// read as short. Thresholds that only warn do not need it.
constexpr uint32_t LARGEST_FREE_BLOCK_SLACK = 16;

constexpr size_t MIN_FREE_HEAP_FOR_TEXT_LAYOUT = EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT = EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC;
constexpr size_t MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD = EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD = EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD_ARENA = EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD_ARENA_BIONIC = EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA_BIONIC;

// heapAllowsTableRowLayout() guards the row-layout allocations (cell wrapping allocates
// TextBlock vectors). It deliberately reuses the TEXT LAYOUT thresholds above rather than
// carrying its own: a bespoke
// 20 KB free-only floor used to live here, which was wrong twice over. It ignored
// getMaxAllocHeap(), unlike every other gate in this file, while the allocations it guards are
// contiguous ones and contiguous runs far below free on this device (a device trace of this
// chapter: free=41244 contig=20468). And 20 KB free was STRICTER than the gate on paragraph
// layout, which is strictly more work -- ensureHeapForTextLayout lays out at 18 KB/12 KB and
// keeps going in degraded mode down to 9 KB/6 KB. Under pressure that inversion sacrificed the
// table on a page while never losing a word of the prose beside it, which is exactly how
// appendix-b of Roosevelt's "Through the Brazilian Wilderness" lost its tables.
//
// Sampled ONLY here, immediately before the allocations. The old copy of this check at <td>
// open sampled while the paragraph machinery was at ITS peak (a full page of PageLines awaiting
// emit), so it read another subsystem's trough and degraded a row that could afford itself. The
// deterministic buffered-byte budget below is what bounds a row as it accumulates.

// Size bound on the buffered row (the gate that actually fires in time).
//
// A heap sample cannot bound a row that grows into the margin. This file used to take one at
// every <td> as well; it could not: X3 alice spine 2 opened its table at ~30 KB free, passed
// every per-cell check, and was at 9776 free / 5364 max alloc by the time </table> drained it as
// paragraphs -- 20 KB consumed in 61 ms with no switch. By construction a heap check only trips
// once enough memory is ALREADY committed to buffers that still have to be drained; on a
// fragmented session (contig 15860 instead of 27636) the same table hard-abort()ed instead of
// degrading. That per-<td> sample is gone; the pre-allocation one in commitPendingRow() remains.
//
// So bound the quantity we can actually measure as it accumulates -- attributed buffered bytes --
// instead of sampling the heap. Deterministic, book-independent, and it fires BEFORE the memory
// is committed. It can act at the next <td>, or with a cell still open via
// degradeRowAtOpenCell(), which is what a one-cell row needs.
#ifndef EHP_TABLE_BUFFER_BUDGET_BYTES
#define EHP_TABLE_BUFFER_BUDGET_BYTES (12 * 1024)
#endif
// Now a per-ROW budget: rows are laid out and freed at </tr>, so the table as a whole no longer
// accumulates. The budget stays because streaming bounds residency at table scope, not at row
// scope -- one row of 8 cells each holding the 189-word cell that caused the original X3 abort()
// is ~72 KB, and nothing else bounds it. Keeping the same number makes this strictly tighter than
// the whole-table budget it replaces, and it fires far more rarely.
constexpr size_t MAX_TABLE_ROW_BUFFER_BYTES = EHP_TABLE_BUFFER_BUDGET_BYTES;

// Attributed cost of one buffered word: the std::string header in ParsedText::words (24 B under
// 32-bit libstdc++) plus the parallel per-word vectors (wordStyles, wordSizes, wordBoundaries) and
// the geometric growth headroom all four carry. Deliberately a fixed constant rather than
// sizeof(std::string) so host goldens and device builds make the SAME switch decision -- a
// host-derived width (32 B) would charge more per word and trip earlier than the device.
constexpr size_t TABLE_BUFFER_BYTES_PER_WORD = 48;
// Words past the SSO capacity additionally allocate their own heap block.
constexpr size_t TABLE_BUFFER_SSO_CAPACITY = 15;
// One buffered cell: BufferedTableCell (two std::string members, colSpan/isHeader, the owning
// pointer) plus the heap-allocated ParsedText and its four empty vectors.
constexpr size_t TABLE_BUFFER_BYTES_PER_CELL = 128;

// The separate 96-word bound on a single cell is gone. It existed only because the old whole-table
// budget could not act with a cell open, so a one-cell table -- X3 alice spine 2's
// <table class="rabbithole">, ONE row, ONE cell, 189 words / 9200 attributed bytes -- had no next
// <td> at which to switch, and buffered to the hard floor. The row budget is charged per word and
// acts with a cell open via degradeRowAtOpenCell(), so it covers that case directly.

// "caption" belongs here even though it only ever appears inside a <table>: it is a block in
// its own right, and without block treatment its text joins whatever inline context preceded
// the table instead of getting its own style, spacing and font-size normalization.
const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "pre", "caption"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

// Elements whose background picture becomes a picture of its own (noteBackgroundImage). Containers
// only: on body/html a background is a page texture, on li/span/a a bullet, an icon or an ornament.
const char* BACKGROUND_PICTURE_TAGS[] = {"div",   "p",          "table",   "section", "figure",
                                         "aside", "blockquote", "article", "center"};
constexpr int NUM_BACKGROUND_PICTURE_TAGS = sizeof(BACKGROUND_PICTURE_TAGS) / sizeof(BACKGROUND_PICTURE_TAGS[0]);
// A smaller background picture is decoration -- a flourish, a rule, a corner -- not an illustration.
constexpr uint16_t MIN_BACKGROUND_PICTURE_SIDE = 96;
// Background scopes open at once; a deeper one is ignored rather than tracked.
constexpr size_t MAX_BACKGROUND_PICTURE_SCOPES = 4;

// Elements whose own horizontal inset also applies to the blocks nested inside them
// (blockInsetStack_). Everything that can hold another block belongs here, plus <p>/<li>/<pre>,
// whose <br>-separated lines each become a block of their own and must keep the inset of the
// paragraph they belong to. <ul>/<ol> are how a list's items get their indent: in CSS an item
// starts at the list's content edge, and its own margin-left only adds to that.
const char* INSET_CONTAINER_TAGS[] = {"div", "blockquote", "section", "article", "aside", "main",
                                      "p",   "li",         "pre",     "ul",      "ol"};
constexpr int NUM_INSET_CONTAINER_TAGS = sizeof(INSET_CONTAINER_TAGS) / sizeof(INSET_CONTAINER_TAGS[0]);

// A list's padding-left when the book states none -- the stand-in for the browser default
// `padding-inline-start: 40px`, which is what indents an unstyled list. Nested lists each add it.
constexpr float LIST_DEFAULT_PADDING_EM = 1.5f;

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* STRIKETHROUGH_TAGS[] = {"s", "del", "strike"};
constexpr int NUM_STRIKETHROUGH_TAGS = sizeof(STRIKETHROUGH_TAGS) / sizeof(STRIKETHROUGH_TAGS[0]);

const char* IMAGE_TAGS[] = {"img", "image"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// Length in bytes of the Default_Ignorable UTF-8 sequence starting at s[i], or 0 if there
// isn't one. Every BMP default-ignorable encodes as 2 bytes (U+034F, U+061C) or 3 bytes, so
// only those two forms are decoded.
//
// A sequence that runs past `len` returns 0 rather than matching on a prefix: the text handler
// is fed in chunks and may split a codepoint at the boundary. Answering "not ignorable" there
// is the safe direction — the bytes stay in the word buffer and the next chunk completes them,
// which is exactly what the U+00A0 / U+202F checks further down already do.
int defaultIgnorableLen(const char* s, const int i, const int len) {
  const auto b0 = static_cast<uint8_t>(s[i]);
  // All default-ignorables are >= U+034F, whose lead byte is 0xCD. Anything below that (ASCII,
  // Latin-1 supplement, Latin Extended-A) cannot be one, and that is nearly every byte here.
  if (b0 < 0xCD) return 0;

  if ((b0 & 0xE0) == 0xC0) {  // 2-byte form
    if (i + 1 >= len) return 0;
    const auto b1 = static_cast<uint8_t>(s[i + 1]);
    if ((b1 & 0xC0) != 0x80) return 0;
    const uint32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
    return utf8IsDefaultIgnorable(cp) ? 2 : 0;
  }

  if ((b0 & 0xF0) == 0xE0) {  // 3-byte form
    if (i + 2 >= len) return 0;
    const auto b1 = static_cast<uint8_t>(s[i + 1]);
    const auto b2 = static_cast<uint8_t>(s[i + 2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
    const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
    return utf8IsDefaultIgnorable(cp) ? 3 : 0;
  }

  return 0;
}

bool hasAttributeToken(const char* value, const char* token) {
  if (!value || !token) return false;
  const size_t tokenLen = strlen(token);
  const char* cursor = value;
  while (*cursor != '\0') {
    while (*cursor != '\0' && isWhitespace(*cursor)) ++cursor;
    const char* start = cursor;
    while (*cursor != '\0' && !isWhitespace(*cursor)) ++cursor;
    if (static_cast<size_t>(cursor - start) == tokenLen && strncmp(start, token, tokenLen) == 0) return true;
  }
  return false;
}

// Returns true if the trailing UTF-8 codepoint in [buf, buf+len) is a dash that allows
// a line break opportunity after it. Inline-tag boundaries like "gone—<i>Umbriel</i>"
// would otherwise glue the dash to the following word via nextWordContinues, making the
// dash unbreakable; callers use this to skip setting that flag when the buffered text
// already ends at a natural break point.
//
// Soft hyphen (U+00AD) and non-breaking hyphen (U+2011) are intentionally excluded:
// soft hyphen is invisible (a hyphenation hint) and non-breaking hyphen forbids breaks
// by definition. Minus sign (U+2212) is excluded because it's mathematical, not a word
// separator.
bool bufferEndsWithBreakableDash(const char* buf, const int len) {
  if (len <= 0) return false;
  int start = len - 1;
  while (start > 0 && (static_cast<uint8_t>(buf[start]) & 0xC0) == 0x80) {
    --start;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(buf + start);
  const uint32_t cp = utf8NextCodepoint(&ptr);
  switch (cp) {
    case '-':
    case 0x2010:  // HYPHEN
    case 0x2012:  // FIGURE DASH
    case 0x2013:  // EN DASH
    case 0x2014:  // EM DASH
    case 0x2015:  // HORIZONTAL BAR
    case 0x2E3A:  // TWO-EM DASH
    case 0x2E3B:  // THREE-EM DASH
      return true;
    default:
      return false;
  }
}

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

std::string ChapterHtmlSlimParser::abbreviateInlineFootnote(const char* text) const {
  if (!text || *text == '\0') return {};
  const int maxAdvance = static_cast<int>(viewportWidth) * 2;
  const int spaceAdvance = renderer.getSpaceWidth(fontId);
  int usedAdvance = 0;
  std::string result;
  const char* cursor = text;
  while (*cursor != '\0') {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') break;
    const char* wordStart = cursor;
    while (*cursor != '\0' && *cursor != ' ') ++cursor;
    const std::string previewWord(wordStart, static_cast<size_t>(cursor - wordStart));
    const int wordAdvance = renderer.getTextWidth(fontId, previewWord.c_str());
    const int separatorAdvance = result.empty() ? 0 : spaceAdvance;
    if (!result.empty() && usedAdvance + separatorAdvance + wordAdvance > maxAdvance) {
      result += "...";
      break;
    }
    if (!result.empty()) result += ' ';
    result += previewWord;
    usedAdvance += separatorAdvance + wordAdvance;
  }
  return result;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

// Returns true if the HTML element is a purely inline wrapper whose ids are mostly not
// navigation targets. Reading-system converters (Kobo KePub, Calibre, etc.) frequently inject
// thousands of such IDs for progress tracking or internal bookkeeping -- but some books do put
// their note targets on one, so these ids are recorded under a budget of their own
// (ChapterHtmlSlimParser::recordSpanAnchor) rather than dropped.
// Block-level, sectioning, and structural elements are always considered navigable.
bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

std::string buildTextBlockPreview(const TextBlock* line, const size_t maxLen = 120) {
  if (!line) {
    return {};
  }

  std::string preview;
  const uint16_t wordCount = line->wordCount();
  for (uint16_t i = 0; i < wordCount; ++i) {
    if (i > 0) {
      preview.push_back(' ');
    }
    preview += line->wordText(i);
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

// Calibre sometimes injects empty <p style="margin:0; border:0; height:0">...</p>
// spacers inside running prose. Keep them as paragraph boundaries, but ignore
// their inner text payload (usually NBSP) to avoid no-break-space glue artifacts.
bool isZeroHeightSpacerParagraph(const char* name, const std::string& styleAttr) {
  if (strcmp(name, "p") != 0 || styleAttr.empty()) {
    return false;
  }

  std::string normalized;
  normalized.reserve(styleAttr.size());
  for (const char ch : styleAttr) {
    if (!isWhitespace(ch)) {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  const bool hasZeroHeight = normalized.find("height:0") != std::string::npos;
  const bool hasZeroMargin = normalized.find("margin:0") != std::string::npos;
  const bool hasZeroBorder = normalized.find("border:0") != std::string::npos;
  return hasZeroHeight && hasZeroMargin && hasZeroBorder;
}

namespace {
// Half-width, in percent, of the "this is really body text" band around 100%.
// Wide when font-size normalization is on (publisher near-body sizing snaps to native
// glyphs), otherwise a tight float-rounding cleanup. Shared by the per-word channel
// (updateEffectiveInlineStyle) and the block channel (normalizeFontSizeForElement) so a
// publisher's near-body size is treated the same whichever one it arrives on.
constexpr int nearBodyDeadZonePct(const bool normalizationEnabled) { return normalizationEnabled ? 10 : 3; }
}  // namespace

// Update effective bold/italic/underline from the style stack. Blocks are on it too
// (pushBlockStyleEntry), so a block's emphasis reaches the blocks nested inside it.
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  effectiveBold = false;
  effectiveItalic = false;
  effectiveUnderline = false;
  effectiveStrikethrough = false;
  effectiveSup = false;
  effectiveSub = false;
  effectiveSmallCaps = false;
  effectiveInlineMarginLeft = 0;
  // Inline font-size composes multiplicatively through the stack (em is relative to
  // the parent element); the block's own font-size lives in BlockStyle, so 100 here
  // means "the block size". Tracked in integer percent to match the per-word channel.
  int sizePct = 100;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasStrikethrough) {
      effectiveStrikethrough = entry.strikethrough;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if (entry.hasSmallCaps) {
      effectiveSmallCaps = entry.smallCaps;
    }
    if (entry.hasMarginLeft) {
      effectiveInlineMarginLeft = entry.marginLeftPx;
    }
    if (entry.hasFontSize) {
      sizePct = sizePct * entry.fontSizePct / 100;
    }
  }
  // Snap composed word sizes near body to plain body text. When font-size normalization is
  // enabled the band is wide (±10%): publishers routinely wrap whole paragraphs in a
  // <span style="font-size:0.92em"> (etc.), where the per-glyph shrink is below the size
  // just-noticeable threshold yet tiring across a page and forces per-glyph resampling.
  // Snapping here — before any block split — keeps such text native regardless of length,
  // and keeps these lines on the zero-cost uniform paths (no per-word size array, no
  // scaled draws). Genuinely distinct sizes (footnotes ~0.8, captions, sup/sub) fall
  // outside the band and survive. Deliberate <10% per-word gradients lose their faintest
  // steps, an accepted trade for body-text comfort. When disabled, only a tight ±3% band
  // is applied (float-rounding cleanup), so publisher near-body wrappers are preserved.
  const int deadZone = nearBodyDeadZonePct(fontSizeNormalization);
  if (sizePct >= 100 - deadZone && sizePct <= 100 + deadZone) {
    sizePct = 100;
  }
  effectiveSizePct = static_cast<uint8_t>(
      std::min<int>(std::max<int>(sizePct, ParsedText::MIN_WORD_SIZE_PCT), ParsedText::MAX_WORD_SIZE_PCT));
}

void ChapterHtmlSlimParser::applyCssFontSizeToEntry(StyleStackEntry& entry, const CssStyle& cssStyle) {
  if (!cssStyle.hasFontSizeMultiplier()) return;
  const int pct = static_cast<int>(cssStyle.fontSizeMultiplier * 100.0f + 0.5f);
  entry.hasFontSize = true;
  entry.fontSizePct = static_cast<uint8_t>(
      std::min<int>(std::max<int>(pct, ParsedText::MIN_WORD_SIZE_PCT), ParsedText::MAX_WORD_SIZE_PCT));
}

void ChapterHtmlSlimParser::applyCssEmphasisToEntry(StyleStackEntry& entry, const CssStyle& cssStyle) {
  if (cssStyle.hasFontWeight()) {
    entry.hasBold = true;
    entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  }
  if (cssStyle.hasFontStyle()) {
    entry.hasItalic = true;
    entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  }
  if (cssStyle.hasTextDecoration()) {
    const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
    if (dec == static_cast<uint8_t>(CssTextDecoration::None)) {
      entry.hasUnderline = true;
      entry.underline = false;
      entry.hasStrikethrough = true;
      entry.strikethrough = false;
    } else {
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
      }
    }
  }
  if (cssStyle.hasSmallCaps()) {
    entry.hasSmallCaps = true;
    entry.smallCaps = cssStyle.smallCaps;
  }
}

// A block's emphasis used to sit in one slot (currentCssStyle) that each nested block overwrote
// and each block end cleared, so <div style="font-style:italic"><p>..</p>tail</div> came out
// upright, and the slot sat under the stack, so an italic <section> beat its own <p
// style="font-style:normal">. On the stack, in document order, both inherit and override as in
// CSS. Based on crosspoint-reader PR #3891 (Justin Mitchell, @itsthisjustin), whose
// pushBlockTextStyleEntry does the same for every block.
void ChapterHtmlSlimParser::pushBlockStyleEntry(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasTextDecoration() &&
      !cssStyle.hasSmallCaps()) {
    return;
  }
  StyleStackEntry entry;
  entry.depth = depth;
  applyCssEmphasisToEntry(entry, cssStyle);
  inlineStyleStack.push_back(entry);
}

void ChapterHtmlSlimParser::applySupSubDefaultSize(StyleStackEntry& entry) {
  if ((entry.hasSup && entry.sup) || (entry.hasSub && entry.sub)) {
    entry.hasFontSize = true;
    entry.fontSizePct = kSupSubDefaultSizePct;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& cssStyle) {
  if (!cssStyle.hasVerticalAlign()) return;
  if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  } else {
    // baseline: explicitly cancel any inherited sup/sub
    entry.hasSup = true;
    entry.sup = false;
    entry.hasSub = true;
    entry.sub = false;
  }
}

namespace {
bool isRootFontSizeElement(const char* tagName) { return strcmp(tagName, "html") == 0 || strcmp(tagName, "body") == 0; }

float saneFontSizeBaseline(float value) {
  if (value < 0.25f || value > 4.0f) return 1.0f;
  return value;
}
}  // namespace

void ChapterHtmlSlimParser::initializeFontSizeBaseline() {
  if (!cssParser) return;

  if (!hasRootFontSizeBaseline_) {
    const CssStyle bodyStyle = cssParser->resolveStyle("body", "");
    if (bodyStyle.hasFontSizeMultiplier() && bodyStyle.fontSizeMultiplier != 1.0f) {
      rootFontSizeBaseline_ = saneFontSizeBaseline(bodyStyle.fontSizeMultiplier);
      hasRootFontSizeBaseline_ = rootFontSizeBaseline_ != 1.0f;
    } else {
      const CssStyle htmlStyle = cssParser->resolveStyle("html", "");
      if (htmlStyle.hasFontSizeMultiplier() && htmlStyle.fontSizeMultiplier != 1.0f) {
        rootFontSizeBaseline_ = saneFontSizeBaseline(htmlStyle.fontSizeMultiplier);
        hasRootFontSizeBaseline_ = rootFontSizeBaseline_ != 1.0f;
      }
    }
  }

  if (!hasMainTextFontSizeBaseline_) {
    const CssStyle paragraphStyle = cssParser->resolveStyle("p", "");
    if (paragraphStyle.hasFontSizeMultiplier() && paragraphStyle.fontSizeMultiplier != 1.0f) {
      mainTextFontSizeBaseline_ = saneFontSizeBaseline(paragraphStyle.fontSizeMultiplier);
      hasMainTextFontSizeBaseline_ = mainTextFontSizeBaseline_ != 1.0f;
      return;
    }

    const CssStyle listStyle = cssParser->resolveStyle("li", "");
    if (listStyle.hasFontSizeMultiplier() && listStyle.fontSizeMultiplier != 1.0f) {
      mainTextFontSizeBaseline_ = saneFontSizeBaseline(listStyle.fontSizeMultiplier);
      hasMainTextFontSizeBaseline_ = mainTextFontSizeBaseline_ != 1.0f;
    }
  }
}

void ChapterHtmlSlimParser::observeFontSizeBaseline(const char* tagName, const CssStyle& cssStyle) {
  if (!cssStyle.hasFontSizeMultiplier()) return;

  // Only the root context (html/body, including class/inline sizing such as the
  // Calibre `body.calibreN { font-size: … }` wrapper) is observed live. The
  // main-text baseline stays tag-level (see initializeFontSizeBaseline): a
  // class-styled paragraph like a decorative opener is indistinguishable here
  // from ordinary prose, so treating the first sized <p> as the baseline would
  // wrongly shrink/grow the real body text.
  if (!hasRootFontSizeBaseline_ && isRootFontSizeElement(tagName) && cssStyle.fontSizeMultiplier != 1.0f) {
    rootFontSizeBaseline_ = saneFontSizeBaseline(cssStyle.fontSizeMultiplier);
    hasRootFontSizeBaseline_ = rootFontSizeBaseline_ != 1.0f;
  }
}

CssStyle ChapterHtmlSlimParser::normalizeFontSizeForElement(const char* tagName, const CssStyle& cssStyle) const {
  if (!cssStyle.hasFontSizeMultiplier()) return cssStyle;

  CssStyle normalized = cssStyle;
  if (hasRootFontSizeBaseline_ && isRootFontSizeElement(tagName)) {
    normalized.fontSizeMultiplier = 1.0f;
  }
  const bool blockLevel = isHeaderOrBlock(tagName) && strcmp(tagName, "br") != 0;
  if (hasMainTextFontSizeBaseline_ && blockLevel) {
    normalized.fontSizeMultiplier /= mainTextFontSizeBaseline_;
  }
  // Block twin of the per-word near-body snap in updateEffectiveInlineStyle. The same
  // publisher sizing arrives either as a <span style="font-size:1.1em"> wrapping the
  // paragraph or as the paragraph's own class (`p.body { font-size: 1.1em }`), and the
  // two must land identically — otherwise a whole book's prose renders as resampled
  // glyphs (or, via FontSizeLadder, on the wrong sibling font) with normalization on.
  // Applied here, on the element's own size, so a nested run keeps its size relative to
  // the now-body block: a 0.8 footnote span inside a 1.1 paragraph resolves to 0.8, not
  // 0.88. Headings are exempt: a heading is meant to stand apart from the prose, so the
  // size its author gave it is kept even when the margin is slim — the point of the band
  // is to spare BODY text a scale nobody asked for, not to flatten the page's hierarchy.
  // (The main-text baseline division above still applies to them, as it always has.)
  if (blockLevel && !matches(tagName, HEADER_TAGS, NUM_HEADER_TAGS)) {
    const int deadZone = nearBodyDeadZonePct(fontSizeNormalization);
    // Rounded to integer percent like the per-word channel (applyCssFontSizeToEntry), so the
    // same declared size lands the same side of the band edge on either channel — a float
    // 1.1em multiplies out a hair either side of 110.
    const int pct = static_cast<int>(normalized.fontSizeMultiplier * 100.0f + 0.5f);
    if (pct >= 100 - deadZone && pct <= 100 + deadZone) {
      normalized.fontSizeMultiplier = 1.0f;
    }
  }
  return normalized;
}
CssStyle ChapterHtmlSlimParser::inlineStyleFor(const std::string& styleAttr) {
  const auto it = inlineStyleCache_.find(styleAttr);
  if (it != inlineStyleCache_.end()) return it->second;
  CssStyle parsed = CssParser::parseInlineStyle(styleAttr);
  if (styleMemoHasRoom(inlineStyleCache_)) inlineStyleCache_.emplace(styleAttr, parsed);
  return parsed;
}

CssStyle ChapterHtmlSlimParser::resolvedImgStyle(const std::string& classAttr) {
  std::string key("img|");
  key += classAttr;
  const auto it = cssStyleCache_.find(key);
  if (it != cssStyleCache_.end()) return it->second;
  CssStyle resolved = cssParser->resolveStyle("img", classAttr);
  if (styleMemoHasRoom(cssStyleCache_)) cssStyleCache_.emplace(std::move(key), resolved);
  return resolved;
}

bool ChapterHtmlSlimParser::ensureHeapForTextLayout(const char* phase, const ParsedText* block) {
  if (streamFailed) {
    return false;
  }

  // A block that could not grow its word vectors is already missing words; laying it out would
  // write a silently truncated paragraph into the cache. Same exit as the floors below.
  if (block != nullptr && block->wordGrowthRefused()) {
    LOG_ERR("EHP", "Word vector growth refused (%u free, %u max alloc), aborting parse before %s", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), phase);
    streamFailed = true;
    layoutFailed = true;
    saxParser_.stop();
    return false;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT && maxAllocHeap >= MIN_MAX_ALLOC_FOR_TEXT_LAYOUT) {
    return true;
  }

  // Soft low-memory zone: keep parsing in degraded mode and only hard-abort when
  // both free and contiguous heap fall to critical levels.
  //
  // The contiguous floor carries the same slack the comment below describes, because on THIS
  // branch being a few bytes short does not merely warn -- it falls through to streamFailed and
  // saxParser_.stop(), and the half-built section is written to cache. Device log, appendix-b:
  //
  //   Table row layout skipped (16864 free, 6132 max alloc); row falls back to paragraphs
  //   [ERR] Low heap (16596 free, 6132 max alloc), aborting parse before table cell paragraph
  //   [ERR] Parse incomplete; keeping partial section cache with 30 pages
  //
  // 6132 against a 6144 floor. Free heap was 16596, nearly double its own floor of 9216; nothing
  // was actually exhausted. Losing half a chapter to twelve bytes of allocator bookkeeping is the
  // one outcome this gate exists to prevent.
  // See EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC_ARENA: with line bytes in the arena the heap floor
  // only has to cover the per-paragraph vectors.
  const size_t hardContigFloor = buildArena_ == nullptr ? MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD
                                 : bionicReadingEnabled ? MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD_ARENA_BIONIC
                                                        : MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD_ARENA;
  if (freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD && maxAllocHeap >= hardContigFloor - LARGEST_FREE_BLOCK_SLACK) {
    // Deliberately does NOT latch image handling off any more. This gate trips on a transient dip
    // — and trips often, because the soft floor (12 * 1024) is a value the allocator can never
    // report: every largest-free-block it returns is 512k - 12, so the neighbours are 12276 and
    // 12788 and the effective floor is the latter. A single 12-bytes-short reading used to
    // disable images for the REST OF THE CHAPTER, and because the parse writes the section cache,
    // that alt-text stood in for every later image until the cache was invalidated. Image cost is
    // now judged where it is actually incurred (heapAllowsImageHeaderRead).
    LOG_DBG("EHP", "Low heap (%u free, %u max alloc) before %s; continuing in degraded mode", freeHeap, maxAllocHeap,
            phase);
    return true;
  }

  LOG_ERR("EHP", "Low heap (%u free, %u max alloc), aborting parse before %s", freeHeap, maxAllocHeap, phase);
  streamFailed = true;
  layoutFailed = true;
  saxParser_.stop();
  return false;
}

// The SOFT text-layout thresholds (18 KB free / 12 KB contig), and deliberately NOT the hard pair
// that ensureHeapForTextLayout aborts on.
//
// The obvious-looking argument for the hard pair is wrong, and it was tried on device. It runs:
// the soft floor is only where text layout WARNS, it keeps laying out below that, so gating the
// grid on the soft floor makes a table more fragile than the prose beside it. Every step of that
// is true and the conclusion still destroys the chapter. What it misses is that these two floors
// are not interchangeable -- the higher one is the RESERVE FOR THE LOWER one. The grid path is
// what drives the heap down (it holds every cell's lines at once until the row is packed); the
// paragraph path survives lower because it drains and frees one cell at a time. Gate the grid
// where text layout dies and the grid eats the margin the fallback needs, so when a row finally
// does decline there is nothing left to fall back INTO:
//
//   Table row layout skipped (9436 free, 6132 max alloc); row falls back to paragraphs
//   [ERR] Low heap (9216 free, 6132 max alloc), aborting parse before table cell paragraph
//   createSectionFile spine=18 parse done: pages=33          <-- 70 with the soft pair
//
// That is a truncated section written to cache: half of appendix-b of Roosevelt's "Through the
// Brazilian Wilderness" silently gone. The soft pair costs a handful of degraded rows on that
// same chapter and keeps every page.
//
// (Note the 6132 against a 6144 bar -- 12 short. Largest-free-block readings come back 12 bytes
// under the round number, so a threshold on a power of two is close to unreachable. Another
// reason not to set a bespoke one here.)
//
// See the note by MAX_TABLE_ROW_BUFFER_BYTES for why the heap is sampled here, immediately before
// the allocations, rather than as cells accumulate.
bool ChapterHtmlSlimParser::heapAllowsTableRowLayout() const {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  // Free heap uses the SOFT text-layout floor (see the note by MAX_TABLE_ROW_BUFFER_BYTES for why
  // not the hard one). The contiguous bar is deliberately NOT the matching soft value.
  //
  // It was, briefly, and it made tables disappear completely. MIN_MAX_ALLOC_FOR_TEXT_LAYOUT
  // (12 KB) guards laying out a whole PARAGRAPH; a table cell line allocates one TextBlock word
  // arena, tens to a few hundred bytes. Requiring 12 KB contiguous to place a 200-byte line is
  // disproportionate, and it is fatal on the Background-C path, where the secondary buffer is lent
  // to the build arena and contig sits structurally at 8-12 KB: every row of every table was
  // refused while free heap sat at a comfortable 22-33 KB. Every measurement that missed this ran
  // blocking with the framebuffer released, where contig is 30-57 KB.
  //
  // So the contiguous bar is the HARD floor -- the order of magnitude a row actually allocates --
  // while the free-heap bar stays soft and keeps the margin the paragraph fallback needs. The
  // slack term is the allocator's own bookkeeping: largest-free-block readings land a few bytes
  // under the round number (a row was once refused at 12276 against a 12288 bar, twelve short,
  // with the memory plainly there), so neither bar sits on a power of two.
  //
  // Arena builds (buildArena_ set): the cell lines' bytes come from the lent region (layoutTableRow
  // sets the cells' line arena), so what a grid row takes from the heap is the TextBlock objects,
  // the row/cell vectors and the cells' layout scratch -- a few KB -- and the free bar drops to
  // the hard text-layout floor plus a margin. The 18 KB soft bar was sized for heap-resident
  // lines; kept there it demoted every row of the Roosevelt appendix on the X3 at 11-18 KB free
  // while 38 KB of the arena sat idle (X3, 2026-09-26).
  // The next X3 run: with the rows in the arena the first row of the Roosevelt appendix was still
  // refused, 192 bytes under a 12 KB bar, and that one refusal cost a released rebuild. A grid
  // row's heap share is now the TextBlock objects and the row/cell vectors, ~2-3 KB, so the bar
  // sits one KB above the floor at which the parse itself gives up.
  const uint32_t freeFloor = buildArena_ ? MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD + 1024 : MIN_FREE_HEAP_FOR_TEXT_LAYOUT;
  const uint32_t contigFloor =
      (buildArena_ ? MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD_ARENA : MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD) -
      LARGEST_FREE_BLOCK_SLACK;
  const bool ok = freeHeap >= freeFloor && maxAllocHeap >= contigFloor;
  if (!ok) {
    LOG_DBG("EHP", "Table row layout skipped (%u free, %u max alloc); row falls back to paragraphs", freeHeap,
            maxAllocHeap);
  }
  return ok;
}

bool ChapterHtmlSlimParser::heapAllowsImageHeaderRead() const {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  // Content-dropping floor (alt text instead of the image), so it carries the allocator slack
  // like every other one in this file: a largest-free-block reading is never the round number.
  const bool ok = freeHeap >= MIN_FREE_HEAP_FOR_IMAGE_HEADER &&
                  maxAllocHeap >= MIN_MAX_ALLOC_FOR_IMAGE_HEADER - LARGEST_FREE_BLOCK_SLACK;
  if (!ok) {
    LOG_DBG("EHP", "Skipping ZIP image-header read (%u free, %u max alloc); image falls back to alt text", freeHeap,
            maxAllocHeap);
  }
  return ok;
}

size_t ChapterHtmlSlimParser::imageWalkBudget() const {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  // The ring is one contiguous block, so contig is the hard bar; free keeps the same margin the
  // header-read gate keeps for the paragraph fallback, on top of what the walk itself takes.
  if (freeHeap <= MIN_FREE_HEAP_FOR_IMAGE_HEADER || maxAllocHeap <= LARGEST_FREE_BLOCK_SLACK) return 1;
  return std::min<size_t>(freeHeap - MIN_FREE_HEAP_FOR_IMAGE_HEADER, maxAllocHeap - LARGEST_FREE_BLOCK_SLACK);
}

bool ChapterHtmlSlimParser::recoverHeapForImageHeader() {
  if (fontCachesReleasedForImageHeader) return false;
  fontCachesReleasedForImageHeader = true;
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t allocBefore = ESP.getMaxAllocHeap();
  // Safe mid-parse: freeStyleMiniData() repoints each style at its stub and leaves the
  // miss handler wired, so glyphs re-fault on demand. Metrics are re-read, not recomputed,
  // so measurement values — and therefore the layout this build produces — are unchanged.
  if (!renderer.releaseFontCaches()) return false;
  LOG_DBG("EHP", "Released font caches for image header: free %u->%u, maxAlloc %u->%u", freeBefore, ESP.getFreeHeap(),
          allocBefore, ESP.getMaxAllocHeap());
  return true;
}

// flush the contents of partWordBuffer to currentTextBlock
bool ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (streamFailed) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return false;
  }
  // What this places ahead of the word (its list marker, a pending float) stands before the word,
  // wherever in the chunk the flush happens. A table row degraded here pins its own offsets.
  const PinnedPlacement atWord(*this, partWordVisibleOffset_);

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;
  const bool isStrikethrough = strikethroughUntilDepth < depth || effectiveStrikethrough;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (isStrikethrough) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::STRIKETHROUGH);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }
  if (effectiveSmallCaps) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SMALL_CAPS);
  }

  // The item's first word: its marker goes in front of it.
  emitPendingListMarker();

  // flush the buffer — route to table cell text when inside a <td>/<th>
  partWordBuffer[partWordBufferIndex] = '\0';
  if (currentTableCell && currentTableCell->text) {
    // text is null only for a cell already consumed by the streaming path; currentTableCell is
    // cleared alongside that, so this is belt-and-braces against a future reordering.
    currentTableCell->text->addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveSizePct,
                                    partWordVisibleOffset_);
    // Charge the word against the row budget. Only a row still headed for the grid accumulates —
    // a degraded cell is drained and freed at </td>.
    if (currentTable && !currentTable->degraded && !currentTable->rowDegraded) {
      currentTable->pendingRowBytes += TABLE_BUFFER_BYTES_PER_WORD;
      if (static_cast<size_t>(partWordBufferIndex) > TABLE_BUFFER_SSO_CAPACITY) {
        currentTable->pendingRowBytes += partWordBufferIndex + 1;
      }
      // A row past the budget cannot wait for the next <td> — a one-cell row has none. Drain here,
      // with the cell still open.
      if (currentTable->pendingRowBytes >= MAX_TABLE_ROW_BUFFER_BYTES) {
        if (!degradeRowAtOpenCell("row size budget")) {
          partWordBufferIndex = 0;
          nextWordContinues = false;
          return false;
        }
      }
    }
  } else if (currentTextBlock) {
    // If a float image is pending and the block is still empty, attach it now so the
    // first word (and all subsequent words) are laid out beside the image.
    // This handles <p><img style="float:left"/>text...</p>: pendingInlineImage_ is set
    // while the block is empty, but no startNewTextBlock() fires before the first word.
    if (pendingInlineImage_.active && currentTextBlock->isEmpty()) {
      attachPendingFloatImage(currentTextBlock->getBlockStyle());
    }
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveSizePct,
                              partWordVisibleOffset_);

    if (currentTextBlock->size() > 96) {
      if (!ensureHeapForTextLayout("long-block split", currentTextBlock.get())) {
        partWordBufferIndex = 0;
        nextWordContinues = false;
        return false;
      }
      LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
      auto& splitBlockStyle = currentTextBlock->getBlockStyle();
      // First layout of this block happens here, so resolve its font now; the later
      // makePages() call for the remainder is a no-op via fontResolved. Per-word sizes
      // are NOT folded mid-block: more words with the same span size may still stream
      // in after this flush, and folding now would double-scale them.
      resolveBlockFont(splitBlockStyle);
      // Before the float code below reads currentPageNextY: the first line starts under the spacing.
      if (!applyBlockTopSpacing()) {
        currentTextBlock.reset();
        partWordBufferIndex = 0;
        nextWordContinues = false;
        return false;
      }

      // A long paragraph (>96 words) beside a tall float lays out here, bypassing
      // makePages(). Inject the active float so it keeps wrapping in this mid-block
      // flush too — otherwise its first chunk renders full-width over the image.
      const bool splitIsOriginating = static_cast<bool>(deferredPageImage_) || static_cast<bool>(deferredDropCapLine_);
      if (!splitIsOriginating && activeFloatBottom_ > 0 && currentPageNextY < activeFloatBottom_ &&
          splitBlockStyle.floatZoneCount == 0) {
        auto& z = splitBlockStyle.floatZones[splitBlockStyle.floatZoneCount++];
        z.top = activeFloatTop_;  // absolute image coordinates (no re-anchor below)
        z.bottom = activeFloatBottom_;
        z.width = activeFloatWidth_;
        z.isRight = activeFloatIsRight_;
      }

      const int horizontalInset = splitBlockStyle.totalHorizontalInset();
      const uint16_t effectiveWidth =
          (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
      const int splitLineHeight = (splitBlockStyle.floatZoneCount > 0) ? effectiveLineHeight(splitBlockStyle) : 0;
      // Re-anchor only the originating block's zone to the first line; injected
      // zones already carry absolute image coordinates and must not be moved.
      if (splitIsOriginating && splitBlockStyle.floatZoneCount > 0) {
        for (int zi = 0; zi < splitBlockStyle.floatZoneCount; ++zi) {
          const int imgH = splitBlockStyle.floatZones[zi].bottom - splitBlockStyle.floatZones[zi].top;
          splitBlockStyle.floatZones[zi].top = static_cast<int16_t>(currentPageNextY);
          splitBlockStyle.floatZones[zi].bottom = static_cast<int16_t>(currentPageNextY + imgH);
        }
        // As in makePages(): the top spacing moved the float below where it was attached, and the
        // blocks after this one wrap against the active-float extent, so it must move too.
        activeFloatTop_ = static_cast<int16_t>(currentPageNextY);
        activeFloatBottom_ = static_cast<int16_t>(
            currentPageNextY + (splitBlockStyle.floatZones[0].bottom - splitBlockStyle.floatZones[0].top));
      }
      currentTextBlock->layoutAndExtractLines(
          renderer, fontId, effectiveWidth,
          [this](std::unique_ptr<TextBlock> textBlock, const bool lineEndsWithHyphenatedWord,
                 const bool suppressHyphenationRetry) {
            return addLineToPage(std::move(textBlock), lineEndsWithHyphenatedWord, suppressHyphenationRetry,
                                 currentTextBlock->lastLineVisibleOffset());
          },
          false, static_cast<int16_t>(currentPageNextY), splitLineHeight);
      // emitPage() clears floatZoneCount mid-layout when the page overflows — that's
      // intentional: lines on the continuation page should not be narrowed for an
      // image that lives on the previous page.
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  return true;
}

void ChapterHtmlSlimParser::noteElementOnPage(const uint32_t offset) {
  if (currentPageVisibleOffsetSet_) return;
  currentPageVisibleOffset_ = offset;
  currentPageVisibleOffsetSet_ = true;
  // Empty pages emitted just before this one start where this element does.
  for (size_t i = paragraphLutPerPage.size() - pageStartsAwaitingElement_; i < paragraphLutPerPage.size(); ++i) {
    paragraphLutPerPage[i].visibleTextOffset = offset;
  }
  pageStartsAwaitingElement_ = 0;
}

// Emit the current page, keeping paragraphLutPerPage and completedPageCount in lockstep.
// Callers must ensure currentPage is non-null and carries content; the helper resets
// currentPage to a fresh Page and zeroes currentPageNextY so the caller can keep building.
void ChapterHtmlSlimParser::emitPage() {
  // A page with no noted element (nothing but spacing) starts where the next element does, which
  // noteElementOnPage fills in. The counter stands in only if no element ever follows; taken now,
  // mid-chunk, it could lie past the next page's first word.
  const bool awaitingElement = !currentPageVisibleOffsetSet_;
  const uint32_t start = awaitingElement ? visibleTextOffset_ : currentPageVisibleOffset_;
  currentPageVisibleOffsetSet_ = false;
  // Both of these borrow elements of the page being handed off, so they must be dropped
  // before it goes -- and the deferred yPos update is moot on a fresh page anyway (a drop
  // cap stays on the emitted page).
  deferredPageImage_ = nullptr;
  deferredDropCapLine_ = nullptr;
  // The page is handed over only once both tables have its entry, so this table, the consumer's
  // and completedPageCount move in lockstep or not at all.
  if (pageTableFull_ || !paragraphLutPerPage.push_back({start, xpathParagraphIndex, xpathListItemIndex})) {
    stopOnFullPageTable();
  } else if (!completePageFn(std::move(currentPage))) {
    paragraphLutPerPage.pop_back();
    stopOnFullPageTable();
  } else {
    if (awaitingElement) pageStartsAwaitingElement_++;  // bounded by MAX_PAGES_PER_SECTION
    completedPageCount++;
  }
  releasePageBlock();  // the page and its lines are gone: rewind their arena bytes
  // The load side refuses a section past this many pages; stop here with the truncated status
  // (the pages so far are kept) rather than build a cache that is rebuilt on every open.
  if (completedPageCount >= Page::MAX_PAGES_PER_SECTION && !streamFailed) {
    noteCapOverflow(kCapPagesPerSection, "pages per section");
    streamFailed = true;
    layoutFailed = true;
    saxParser_.stop();
  }
  currentPage.reset(new (std::nothrow) Page());
  if (currentPage) currentPage->elements.reserve(Page::TYPICAL_ELEMENTS);
  currentPageNextY = 0;
  lastBlockMarginBottom = 0;
  paragraphLinesOnPage_ = 0;

  // A floated image never crosses a page boundary, so any active float ended on the
  // page we just emitted. Clear it and drop stale float zones from the block that
  // continues onto the new page, so its lines are not indented for a prior image.
  activeFloatTop_ = 0;
  activeFloatBottom_ = 0;
  if (currentTextBlock) {
    currentTextBlock->getBlockStyle().floatZoneCount = 0;
  }
}

// Based on crosspoint-reader PR #3891 (Justin Mitchell, @itsthisjustin; the deferral is Uri
// Tauber's), reworked for our parser, which has no block-style stack to carry the flags on.
void ChapterHtmlSlimParser::applyPendingPageBreak() {
  if (!pageBreakPending_) return;
  pageBreakPending_ = false;
  if (currentPage && !currentPage->elements.empty()) emitPage();
}

void ChapterHtmlSlimParser::stopOnFullPageTable() {
  // Destroyed before emitPage rewinds the page block its lines live in.
  currentPage.reset();
  if (pageTableFull_) return;
  pageTableFull_ = true;
  // A chunk of a page table could not be had: what the old std::vector growth turned into an
  // abort -- and, since the same chapter rebuilds from page 0 on the next open, a reboot loop.
  // Stopping here instead keeps the pages so far as a truncated section, like any low-heap stop:
  // the reader shows it with its hint, and a background build discards it for a rebuild with the
  // framebuffer released, which has the heap to finish the chapter. Based on crosspoint-reader
  // PR #3027 (Sidney San Martín, @s4y), whose build likewise keeps its pages when the LUT runs out.
  LOG_ERR("EHP", "Page table full at %d pages (%u free, %u max alloc); stopping the parse", completedPageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  streamFailed = true;
  layoutFailed = true;
  saxParser_.stop();
}

void ChapterHtmlSlimParser::noteCapOverflow(const uint8_t flag, const char* what) {
  if (capOverflowFlags_ & flag) return;
  capOverflowFlags_ |= flag;
  LOG_ERR("EHP", "Fixed-capacity limit exceeded (%s); the chapter is cached simplified", what);
}

void ChapterHtmlSlimParser::recordPageBreakLabel(const std::string& label) {
  if (label.empty()) {
    return;
  }
  // The section file stores the count as a uint16.
  if (pageBreakLabelPages.size() >= 65535) {
    noteCapOverflow(kCapPageLabels, "printed-page labels per chapter");
    return;
  }

  // One label per printed page, so in a book that is one XHTML file these two run to thousands of
  // entries and double into blocks of several KB mid-parse. A label is worth less than the
  // chapter: without the block, drop it rather than abort the build.
  if (!heapReserveFor(pageBreakLabelPages, 1) || !heapReserveFor(pageBreakLabelPool, label.size() + 1)) {
    noteCapOverflow(kCapPageLabels, "printed-page labels: no heap block to grow the record");
    return;
  }

  // Record the printed page label for the current rendered section page.
  // Do not alter pagination; the reader keeps its own page breaks. A label is text from the
  // page list or the markup, never containing a NUL, so the pool's separators are unambiguous.
  pageBreakLabelPages.push_back(static_cast<uint16_t>(completedPageCount));
  pageBreakLabelPool.append(label);
  pageBreakLabelPool.push_back('\0');
}

void ChapterHtmlSlimParser::wireTextBlock() {
  if (!currentTextBlock) return;
  wireParagraphLines(*currentTextBlock);
}

void ChapterHtmlSlimParser::wireParagraphLines(ParsedText& text) {
  text.setLineArena(buildArena_);
  if (buildArena_) {
    text.setBeforeLineHook([this](const uint8_t maxSizePct) { beforeLineHook(maxSizePct); });
  } else {
    text.setBeforeLineHook(nullptr);
  }
}

void ChapterHtmlSlimParser::beforeLineHook(const uint8_t maxSizePct) {
  // The page-fit test of addLineToPage, run before the line is materialised (see
  // ParsedText::beforeLine_): the same arithmetic, so addLineToPage's own test then finds the
  // line fits and everything after it (anchors, footnotes, the hyphenation retry) is unchanged.
  if (currentPage && currentTextBlock) {
    int lineHeight = effectiveLineHeight(currentTextBlock->getBlockStyle());
    if (maxSizePct != 100) {
      lineHeight = lineHeight * maxSizePct / 100;
    }
    if (currentPageNextY + lineHeight > viewportHeight || keepTogetherBreak(lineHeight)) {
      emitPage();
    }
  }
  ensurePageBlock();
}

// Widows and orphans are kept off a page by ending it early, never by moving lines: a placed
// line's bytes live in the page's arena block, rewound when the page is handed off, so a line
// carried over would dangle. The decision runs before each line, from where the line sits in its
// paragraph (ParsedText::LinePlace), so it is the same in beforeLineHook and addLineToPage, and
// it never ends an empty page: a block taller than a page still splits, and nothing loops.
// The idea is crosspoint-reader PR #3891's (Justin Mitchell, @itsthisjustin; Uri Tauber), which
// moves the page's last lines over instead.
bool ChapterHtmlSlimParser::keepTogetherBreak(const int lineHeight, const bool nextLine) const {
  if (!currentPage || !currentTextBlock || lineHeight <= 0) return false;
  if (currentPage->elements.empty() && !nextLine) return false;
  const BlockStyle& style = currentTextBlock->getBlockStyle();
  // A float (image or drop cap) is placed with its paragraph's first line: those keep the
  // ordinary break.
  if (style.floatZoneCount > 0 || deferredPageImage_ || deferredDropCapLine_) return false;
  ParsedText::LinePlace place = currentTextBlock->linePlace();
  int y = currentPageNextY;
  int onPage = place.paragraphStart ? 0 : paragraphLinesOnPage_;
  if (nextLine) {
    // The line after this one, once this one is placed: for the hyphenation retry below.
    if (place.linesLeft == 1) return false;  // this is the paragraph's last
    if (place.linesLeft > 1) place.linesLeft--;
    place.paragraphStart = false;
    y += lineHeight;
    onPage++;
  }
  const int room = viewportHeight - y;
  if (style.keepWithNext) {
    // A heading goes over unless it fits with its bottom spacing and the first lines below it.
    if (!place.paragraphStart || place.linesLeft == 0) return false;
    const int bodyLineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
    const int need = place.linesLeft * lineHeight + std::max<int>(style.marginBottom, 0) +
                     std::max<int>(style.paddingBottom, 0) + kKeepWithNextLines * bodyLineHeight;
    return need > room;
  }
  const int fit = room / lineHeight;  // lines of this height still on the page, this one included
  if (fit <= 0) return false;         // it does not fit at all: the ordinary break
  // Not known yet: the paragraph runs on past this layout pass, so it has lines enough to split.
  const int left = place.linesLeft != 0 ? place.linesLeft : INT16_MAX;
  if (fit >= left) return false;  // the rest of the paragraph fits
  // How many of its lines may stay on this page, from this one, so that kWidows go over.
  const int keep = std::min(fit, left - kWidows);
  // Its first lines move together unless kOrphans of them can stay.
  if (onPage == 0) return keep < kOrphans;
  // Further on, the page ends here once the rest must go over -- unless that leaves an orphan
  // behind. The lines already placed cannot follow, so the widow is then the lesser evil.
  return keep <= 0 && onPage >= kOrphans;
}

void ChapterHtmlSlimParser::ensurePageBlock() {
  if (buildArena_ == nullptr || pageBlock_.valid()) return;
  pageBlock_ = buildArena_->reserveBlock();
}

void ChapterHtmlSlimParser::releasePageBlock() {
  if (!pageBlock_.valid()) return;
  if (!buildArena_->release(pageBlock_)) {
    // Out of LIFO order: something reserved above this block and is still live. Keep the
    // token (overwriting a live one is a contract violation); later pages simply allocate
    // above it and the region fills, at which point lines fall back to the heap.
    LOG_ERR("EHP", "Page arena block could not be released (out of order); region stays claimed");
  }
}

void ChapterHtmlSlimParser::beginExternalPageBreakAnchors() {
  externalPageBreakAnchorPool.clear();
  externalPageBreakAnchorCount = 0;
  externalPageBreakLabelBytes = 0;
  topOfFilePageLabel.clear();
  topOfFilePageLabelEmitted = false;
}

void ChapterHtmlSlimParser::addExternalPageBreakAnchor(const std::string& id, const std::string& label) {
  if (id.empty()) {
    // NCX pageTarget with no fragment (e.g. "OEBPS/c9_split_000.xhtml") — applies to the
    // first rendered page of this chapter. Keep only the first such entry if multiple.
    if (topOfFilePageLabel.empty()) {
      topOfFilePageLabel = label;
      externalPageBreakLabelBytes += label.size() + 1;
    }
    return;
  }
  if (externalPageBreakAnchorCount == UINT16_MAX) return;  // the page list itself is u16-counted
  // A book that is one XHTML file brings its whole page list here, ~20 bytes an entry: this pool
  // doubles into blocks of tens of KB. Without the block, the entry's label is lost, not the build.
  if (!heapReserveFor(externalPageBreakAnchorPool, id.size() + label.size() + 2)) {
    noteCapOverflow(kCapPageLabels, "page-list labels: no heap block to grow the pool");
    return;
  }
  externalPageBreakAnchorPool.append(id);
  externalPageBreakAnchorPool.push_back('\0');
  externalPageBreakAnchorPool.append(label);
  externalPageBreakAnchorPool.push_back('\0');
  externalPageBreakAnchorCount++;
  externalPageBreakLabelBytes += label.size() + 1;
}

void ChapterHtmlSlimParser::endExternalPageBreakAnchors() {
  // Shrinking is a copy into a block of the exact size, which the heap may not have either.
  if (heapHasBlockFor(externalPageBreakAnchorPool.size() + 1)) externalPageBreakAnchorPool.shrink_to_fit();
  // One label per matched anchor plus the top-of-file one: size the record here so it never
  // doubles mid-parse (inline pagebreak markers past this hint still grow it, through
  // recordPageBreakLabel's heap check). Hints: without the blocks the record grows as it goes.
  heapReserveFor(pageBreakLabelPages, externalPageBreakAnchorCount + 1u);
  heapReserveFor(pageBreakLabelPool, externalPageBreakLabelBytes);
}

void ChapterHtmlSlimParser::attachPendingFloatImage(BlockStyle& bs) {
  if (!pendingInlineImage_.active) return;
  if (!currentPage) currentPage.reset(new (std::nothrow) Page());

  const int16_t imgH = pendingInlineImage_.height;
  const int16_t imgW = pendingInlineImage_.width;
  const bool imgIsRight = pendingInlineImage_.isRight;

  // A floated image is never split across a page boundary. If it would not fit in the
  // space left on this page, break first so it floats at the top of a fresh page —
  // its height is capped at one viewport by the float gate, so a fresh page always
  // has room. This avoids the fragile cross-page tile/continuation path entirely and
  // lets the whole image (with text wrapping beside it) live on a single page.
  if (imgH > static_cast<int16_t>(viewportHeight - currentPageNextY) && currentPage && !currentPage->elements.empty()) {
    emitPage();  // resets currentPage + currentPageNextY=0, clears stale float state
  }

  const int16_t imgX = imgIsRight ? static_cast<int16_t>(viewportWidth - imgW) : 0;
  const int16_t top = static_cast<int16_t>(currentPageNextY);

  auto fullImageBlock =
      makeUniqueNoThrow<ImageBlock>(pendingInlineImage_.cachedPath, imgW, imgH, pendingInlineImage_.alt,
                                    epub->getPath(), pendingInlineImage_.epubEntryPath);
  auto pageImage = fullImageBlock ? makeUniqueNoThrow<PageImage>(std::move(fullImageBlock), imgX, top) : nullptr;
  if (!pageImage) {
    LOG_ERR("EHP", "Float image dropped: allocation failed");
    return;
  }
  // The page owns it from here; this is a borrowed pointer purely so the first line of
  // the block can re-base its yPos below. emitPage() clears it before handing the page
  // on, so it never outlives the element it points at.
  deferredPageImage_ = pageImage.get();
  noteElementOnPage(placementOffset());
  currentPage->elements.push_back(std::move(pageImage));

  // Attach the float zone to the originating block (the caption/first paragraph).
  // makePages() re-anchors it to the first line and then propagates it to every
  // following block that overlaps the image, via the active-float state below.
  if (bs.floatZoneCount < BlockStyle::kMaxFloatZones) {
    auto& z = bs.floatZones[bs.floatZoneCount++];
    z.top = top;
    z.bottom = static_cast<int16_t>(top + imgH);
    z.width = static_cast<int16_t>(imgW + 4);
    z.isRight = imgIsRight;
  }
  // Provisional active-float extent; makePages() finalises top/bottom once the
  // originating block's first line (and thus the image top) is positioned.
  activeFloatTop_ = top;
  activeFloatBottom_ = static_cast<int16_t>(top + imgH);
  activeFloatWidth_ = static_cast<int16_t>(imgW + 4);
  activeFloatIsRight_ = imgIsRight;

  pendingInlineImage_.active = false;
  pendingInlineImage_.cachedPath.clear();
  pendingInlineImage_.epubEntryPath.clear();
  pendingInlineImage_.alt.clear();
}

bool ChapterHtmlSlimParser::tryStartDropCapCapture(const CssStyle& cssStyle) {
  // Positional gates first: they are cheap member reads and reject nearly every
  // inline element in the book, so only an element opening a still-empty paragraph
  // reaches the style checks below.
  if (pendingDropCap_.active || deferredDropCapLine_) return false;
  if (currentTable || currentTableCell) return false;
  if (!currentTextBlock || !currentTextBlock->isEmpty() || partWordBufferIndex > 0) return false;
  if (pendingInlineImage_.active || activeFloatBottom_ > 0 || deferredPageImage_) return false;
  if (currentTextBlock->getBlockStyle().floatZoneCount > 0) return false;

  if (!cssStyle.hasCssFloat() || cssStyle.cssFloat != CssFloat::Left) return false;
  if (!cssStyle.hasFontSizeMultiplier() || cssStyle.fontSizeMultiplier < kDropCapMinMultiplier) return false;

  // Compose the cap size: span CSS (em, relative to parent) x enclosing inline spans
  // x the paragraph block's own multiplier — all relative to the body font. Clamped so
  // the cap stays around three text lines tall (KOReader-comparable) and never absurd.
  float mult =
      cssStyle.fontSizeMultiplier * (effectiveSizePct / 100.0f) * currentTextBlock->getBlockStyle().fontSizeMultiplier;
  mult = std::min(std::max(mult, kDropCapMinMultiplier), kDropCapMaxMultiplier);

  // Inherited bold/italic, overridden by the span's own CSS (drop-cap classes commonly
  // reset font-style: normal inside an italic first phrase).
  bool isBold = boldUntilDepth < depth || effectiveBold;
  bool isItalic = italicUntilDepth < depth || effectiveItalic;
  if (cssStyle.hasFontWeight()) isBold = cssStyle.fontWeight == CssFontWeight::Bold;
  if (cssStyle.hasFontStyle()) isItalic = cssStyle.fontStyle == CssFontStyle::Italic;
  EpdFontFamily::Style capStyle = EpdFontFamily::REGULAR;
  if (isBold) capStyle = static_cast<EpdFontFamily::Style>(capStyle | EpdFontFamily::BOLD);
  if (isItalic) capStyle = static_cast<EpdFontFamily::Style>(capStyle | EpdFontFamily::ITALIC);

  pendingDropCap_.active = true;
  pendingDropCap_.depth = depth;
  pendingDropCap_.multiplier = mult;
  pendingDropCap_.style = capStyle;
  pendingDropCap_.textLen = 0;
  return true;
}

void ChapterHtmlSlimParser::finalizePendingDropCap() {
  pendingDropCap_.active = false;
  const int textLen = pendingDropCap_.textLen;
  pendingDropCap_.textLen = 0;
  if (textLen == 0 || !currentTextBlock) return;
  pendingDropCap_.text[textLen] = '\0';

  // Cap font selection. Unlike resolveBlockFont (nearest rung, single aux slot), a drop
  // cap wants the LARGEST real font available: the desired size is far beyond every rung,
  // so every rung step taken in real glyphs cuts the residual upscale factor (and thus
  // pixelation). If the aux slot is already claimed (chapter heading), reuse that font
  // rather than falling back to scaling the small body font — same decompressor budget.
  int32_t capFontId = 0;  // 0 = body font
  float capScale = pendingDropCap_.multiplier;
  if (fontSizeLadder_.native()) {
    // Every size has a face of its own: the cap is drawn at its own size, scaled only above the largest face.
    const FontSizeLadder::Resolved r = fontSizeLadder_.resolve(pendingDropCap_.multiplier * 100.0f);
    capFontId = r.fontId;
    capScale = r.residual;
  } else {
    int best = -1;
    for (int i = 0; i < fontSizeLadder_.count; ++i) {
      const auto& rung = fontSizeLadder_.rungs[i];
      if (rung.sizePct <= 100) continue;
      if (auxFontId_ != 0 && rung.fontId != auxFontId_) continue;
      if (best < 0 || rung.sizePct > fontSizeLadder_.rungs[best].sizePct) best = i;
    }
    if (best >= 0) {
      capFontId = fontSizeLadder_.rungs[best].fontId;
      capScale = pendingDropCap_.multiplier * 100.0f / fontSizeLadder_.rungs[best].sizePct;
      if (auxFontId_ == 0) auxFontId_ = capFontId;
    }
  }
  const int capEffFontId = capFontId != 0 ? capFontId : fontId;

  // Ink metrics: the ascender metric includes internal leading, which at 2-4x scale
  // becomes half a line of blank space. Place and size the cap by actual glyph ink.
  renderer.ensureFontReady(capEffFontId, pendingDropCap_.text);
  int capInkTop = 0;
  int capInkBelow = 0;
  const bool haveInk =
      renderer.getTextInkMetrics(capEffFontId, pendingDropCap_.text, pendingDropCap_.style, &capInkTop, &capInkBelow);
  const int capWidth = renderer.getTextWidthScaled(capEffFontId, pendingDropCap_.text, pendingDropCap_.style, capScale);

  // Unusable cap (missing glyphs, or too wide to leave a text column): render the
  // captured text inline at paragraph size instead so no characters are lost.
  if (!haveInk || capInkTop <= 0 || capWidth <= 0 || capWidth + kDropCapGapPx > viewportWidth / 2) {
    LOG_DBG("EHP", "dropcap '%s': inline fallback (haveInk=%d inkTop=%d width=%d limit=%d)", pendingDropCap_.text,
            haveInk, capInkTop, capWidth + kDropCapGapPx, viewportWidth / 2);
    currentTextBlock->addWord(pendingDropCap_.text, pendingDropCap_.style, false, false,
                              ParsedText::DEFAULT_WORD_SIZE_PCT, pendingDropCap_.visibleOffset);
    nextWordContinues = true;  // "A" + "ll" form one visual word
    return;
  }

  // First-line ink leading of the paragraph: same string measured in the body font,
  // scaled by the paragraph's own multiplier. The cap's ink top is aligned to this.
  int bodyInkTop = 0;
  int bodyInkBelow = 0;
  renderer.ensureFontReady(fontId, pendingDropCap_.text);
  renderer.getTextInkMetrics(fontId, pendingDropCap_.text, EpdFontFamily::REGULAR, &bodyInkTop, &bodyInkBelow);
  const float paraMult = currentTextBlock->getBlockStyle().fontSizeMultiplier;
  const int bodyLeading =
      std::max(0, static_cast<int>((renderer.getFontAscenderSize(fontId) - bodyInkTop) * paraMult + 0.5f));

  const int capAsc = renderer.getFontAscenderSize(capEffFontId);
  const int capLeadScaled = static_cast<int>((capAsc - capInkTop) * capScale + 0.5f);
  const int capInkHeightScaled = static_cast<int>((capInkTop + capInkBelow) * capScale + 0.5f);
  // Zone spans from the line top to just below the cap's ink bottom.
  const int zoneHeight = bodyLeading + capInkHeightScaled + 2;

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    currentPageNextY = 0;
  }
  // A drop cap never crosses a page boundary — break first if it would not fit.
  if (zoneHeight > viewportHeight - currentPageNextY && currentPage && !currentPage->elements.empty()) {
    emitPage();
  }

  BlockStyle capBlockStyle;
  capBlockStyle.headingFontId = capFontId;
  capBlockStyle.fontSizeMultiplier = capScale;
  capBlockStyle.fontResolved = true;
  auto capBlock = makeUniqueNoThrow<TextBlock>(std::vector<std::string>{pendingDropCap_.text}, std::vector<int16_t>{0},
                                               std::vector<EpdFontFamily::Style>{pendingDropCap_.style}, capBlockStyle,
                                               std::vector<uint8_t>{});
  if (!capBlock || !capBlock->valid()) {
    LOG_DBG("EHP", "dropcap '%s': inline fallback (block alloc failed)", pendingDropCap_.text);
    currentTextBlock->addWord(pendingDropCap_.text, pendingDropCap_.style, false, false,
                              ParsedText::DEFAULT_WORD_SIZE_PCT, pendingDropCap_.visibleOffset);
    nextWordContinues = true;
    return;
  }

  const int16_t capX = currentTextBlock->getBlockStyle().leftInset();
  const int16_t top = static_cast<int16_t>(currentPageNextY);
  // The cap draws its baseline at yPos + capAsc*capScale; shifting yPos up by the scaled
  // internal leading (offset by the first line's own small leading) puts the cap's ink
  // top level with the first line's ink top. Provisional; re-based in addLineToPage.
  dropCapYAdjust_ = static_cast<int16_t>(bodyLeading - capLeadScaled);
  auto capLine = makeUniqueNoThrow<PageLine>(std::move(capBlock), capX, static_cast<int16_t>(top + dropCapYAdjust_));
  if (!capLine) {
    LOG_DBG("EHP", "dropcap '%s': inline fallback (PageLine alloc failed)", pendingDropCap_.text);
    currentTextBlock->addWord(pendingDropCap_.text, pendingDropCap_.style, false, false,
                              ParsedText::DEFAULT_WORD_SIZE_PCT, pendingDropCap_.visibleOffset);
    nextWordContinues = true;
    return;
  }
  // Borrowed, like deferredPageImage_ above: the page owns the line.
  deferredDropCapLine_ = capLine.get();
  // Placed at the span's close, after the captured letters were counted: the cap starts at its
  // first letter.
  noteElementOnPage(pendingDropCap_.visibleOffset);
  currentPage->elements.push_back(std::move(capLine));

  BlockStyle& bs = currentTextBlock->getBlockStyle();
  if (bs.floatZoneCount < BlockStyle::kMaxFloatZones) {
    auto& z = bs.floatZones[bs.floatZoneCount++];
    z.top = top;
    z.bottom = static_cast<int16_t>(top + zoneHeight);
    z.width = static_cast<int16_t>(capWidth + kDropCapGapPx);
    z.isRight = false;
  }
  // Provisional active-float extent so short paragraphs hand the zone on to the next
  // block; makePages() finalises top/bottom when the originating block is positioned.
  activeFloatTop_ = top;
  activeFloatBottom_ = static_cast<int16_t>(top + zoneHeight);
  activeFloatWidth_ = static_cast<int16_t>(capWidth + kDropCapGapPx);
  activeFloatIsRight_ = false;
  LOG_DBG("EHP", "dropcap '%s': placed fontId=%d scale=%.2f zone=%dx%d", pendingDropCap_.text, capFontId, capScale,
          capWidth + kDropCapGapPx, zoneHeight);
}

void ChapterHtmlSlimParser::emitPendingListMarker() {
  if (pendingListMarker_[0] == '\0') return;
  if (currentTextBlock)
    currentTextBlock->addWord(pendingListMarker_, EpdFontFamily::REGULAR, false, false,
                              ParsedText::DEFAULT_WORD_SIZE_PCT, placementOffset());
  pendingListMarker_[0] = '\0';
}

void ChapterHtmlSlimParser::holdBottomSpacing(BlockStyle& style, const bool floated) {
  // A block inside a table cell is not the cell's text (see flushPartWordBuffer); leave it as is.
  if (currentTableCell) return;
  if (floated) {
    // A float is out of flow: its bottom margin never moves the blocks after it. Left on the
    // style it would, through the empty-block merge, land under the paragraph beside the float.
    style.marginBottom = 0;
    style.paddingBottom = 0;
    return;
  }
  const int16_t marginBottom = std::max<int16_t>(style.marginBottom, 0);
  const int16_t paddingBottom = std::max<int16_t>(style.paddingBottom, 0);
  if (marginBottom == 0 && paddingBottom == 0) return;
  if (heldBottomSpacing_.size() >= kMaxHeldBottomSpacingDepth) return;
  heldBottomSpacing_.push_back({depth, marginBottom, paddingBottom});
  style.marginBottom = 0;
  style.paddingBottom = 0;
}

void ChapterHtmlSlimParser::applyHeldBottomSpacing(const HeldBottomSpacing& held) {
  if (!currentTextBlock) return;
  if (partWordBufferIndex > 0) flushPartWordBuffer();
  BlockStyle style = currentTextBlock->getBlockStyle();
  if (currentTextBlock->isEmpty()) {
    // Nothing of the element is left to lay out (it was empty, or ended in an image or a table):
    // the spacing goes before whatever follows, which this empty block merges into.
    style.marginTop = std::max(style.marginTop, held.marginBottom);
    style.paddingTop = static_cast<int16_t>(style.paddingTop + held.paddingBottom);
  } else {
    // The last block's own margin-bottom and the element's collapse into the larger of the two.
    style.marginBottom = std::max(style.marginBottom, held.marginBottom);
    style.paddingBottom = static_cast<int16_t>(style.paddingBottom + held.paddingBottom);
  }
  currentTextBlock->setBlockStyle(style);
}

void ChapterHtmlSlimParser::addAncestorInsets(BlockStyle& style, const float emSize) const {
  if (blockInsetStack_.empty()) return;
  int left = style.leftInset();
  int right = style.rightInset();
  for (const auto& entry : blockInsetStack_) {
    left += entry.left;
    right += entry.right;
  }
  const int cap = static_cast<int>(emSize * BlockStyle::MAX_HORIZONTAL_INSET_EM);
  // Only the sum is ever laid out (leftInset()/rightInset()), so the capped total goes on the
  // margin and the padding keeps its own value. Both stay >= 0: fromCssStyle clamps each
  // element's contribution to [0, cap], so the capped total can never fall below the padding.
  style.marginLeft = static_cast<int16_t>(std::min(left, cap) - style.paddingLeft);
  style.marginRight = static_cast<int16_t>(std::min(right, cap) - style.paddingRight);
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  // Base style for the new block — normally the incoming blockStyle, but when falling
  // through from the empty-block merge path (see below) we use the merged style so that
  // accumulated parent-element margins are preserved for the inline-image paragraph.
  const BlockStyle* effectiveBase = &blockStyle;
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      BlockStyle incoming = blockStyle;
      const bool brGapPending = currentTextBlock->getBlockStyle().fromBrElement;
      if (brGapPending) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      BlockStyle merged = currentTextBlock->getBlockStyle().getCombinedBlockStyle(incoming);
      // Preserve only whether the current empty block still represents <br> separators.
      // This lets consecutive <br> accumulate one line each without leaking the flag to real content blocks.
      merged.fromBrElement = blockStyle.fromBrElement;
      // getCombinedBlockStyle drops float zones by design; a float attached to this
      // still-empty block (drop cap or image) must survive the style merge so the
      // text that eventually lands in it wraps beside the float.
      const BlockStyle& prevStyle = currentTextBlock->getBlockStyle();
      merged.floatZoneCount = prevStyle.floatZoneCount;
      for (int zi = 0; zi < prevStyle.floatZoneCount; ++zi) {
        merged.floatZones[zi] = prevStyle.floatZones[zi];
      }
      currentTextBlock->setBlockStyle(merged);

      // Whatever came before is laid out already: the block is empty.
      applyPendingPageBreak();
      if (!pendingAnchorId.empty()) {
        if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
          if (currentPage && !currentPage->elements.empty()) {
            emitPage();
          }
        }
        queueAnchorForNextLine(std::move(pendingAnchorId));
        pendingAnchorId.clear();
      }
      wordsExtractedInBlock = 0;
      // If an inline image is waiting, fall through to place it now rather than
      // returning early — otherwise the image skips empty wrapper blocks and
      // attaches to the *second* paragraph instead of the first.
      if (!pendingInlineImage_.active) return;
      // Fall through: use the merged style as the base so parent-element margins
      // (accumulated into this empty block) are carried into the new paragraph.
      effectiveBase = &currentTextBlock->getBlockStyle();
    }

    if (!currentTextBlock->isEmpty()) makePages();
  }
  applyPendingPageBreak();
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (!pendingAnchorId.empty() &&
      std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      emitPage();
    }
  }
  // Queue the deferred anchor now that the previous block is flushed (and any TOC page break has
  // happened); its PAGE is settled by addLineToPage, once this block's first line is placed.
  if (!pendingAnchorId.empty()) {
    queueAnchorForNextLine(std::move(pendingAnchorId));
    pendingAnchorId.clear();
  }
  // Apply pending inline image: attach float zone and place image on current page.
  // The image's actual yPos will be fixed in addLineToPage once the baseline is known.
  BlockStyle blockStyleWithIndent = *effectiveBase;
  attachPendingFloatImage(blockStyleWithIndent);
  // Reuse the laid-out (now empty) block rather than replacing it: its word vectors keep the
  // capacity the previous paragraph grew them to. See ParsedText::reset.
  if (currentTextBlock) {
    currentTextBlock->reset(blockStyleWithIndent);
  } else {
    currentTextBlock.reset(new (std::nothrow) ParsedText(extraParagraphSpacing, hyphenationEnabled,
                                                         blockStyleWithIndent, bionicReadingEnabled));
  }
  wireTextBlock();
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (VisibleText::isNonVisibleTag(name)) self->nonVisibleTextDepth_++;

  if (self->streamFailed) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Track SVG nesting depth. Must be checked before the svgDepth>0 guard below so that
  // nested <svg> elements increment the counter rather than being swallowed as unknowns.
  if (strcmp(name, "svg") == 0) {
    self->svgDepth += 1;
    self->depth += 1;
    return;
  }

  // Inside SVG: only process <image> elements (raster images); skip everything else.
  // SVG child elements like <path>, <rect>, <circle>, <text> must not reach the layout
  // engine — they would accumulate path data and exhaust heap on large inline SVG.
  if (self->svgDepth > 0 && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    self->depth += 1;
    return;
  }

  // A held-back list marker waits only through the empty-block merge of a <p>/<div> opening the
  // item. Content that is not text -- a nested item, an image, a table, a rule, a line break --
  // takes it at once, so the marker stays in the item's own block, ahead of that content.
  if (self->pendingListMarker_[0] != '\0' &&
      (strcmp(name, "li") == 0 || matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || strcmp(name, "table") == 0 ||
       strcmp(name, "hr") == 0 || strcmp(name, "br") == 0)) {
    self->emitPendingListMarker();
  }

  // Extract class, style, id, hidden, and pagebreak metadata attributes
  std::string classAttr;
  std::string styleAttr;
  std::string idAttr;
  std::string ariaLabel;
  std::string titleAttr;
  bool hasHiddenAttr = false;
  bool isPageBreakMarker = false;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        idAttr = atts[i + 1];
      } else if (strcmp(atts[i], "aria-label") == 0) {
        ariaLabel = atts[i + 1];
      } else if (strcmp(atts[i], "title") == 0) {
        titleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "hidden") == 0) {
        // A boolean attribute: its presence is what counts. HTML says any value,
        // including the empty string and "false", still means hidden.
        hasHiddenAttr = true;
      } else if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0) {
        isPageBreakMarker = true;
      } else if (strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        isPageBreakMarker = true;
      }
    }
  }

  // Emit any "top-of-file" printed-page label as soon as we see real markup. NCX entries
  // without a fragment refer to the start of this XHTML; record now so the label lands on
  // page 0 (completedPageCount is still 0 until the first emitPage()).
  if (!self->topOfFilePageLabelEmitted && !self->topOfFilePageLabel.empty()) {
    self->recordPageBreakLabel(self->topOfFilePageLabel);
    self->topOfFilePageLabelEmitted = true;
  }

  // Match id against NCX-supplied pagebreak anchors (printed page list). If matched,
  // treat this element as if it carried an inline doc-pagebreak marker.
  std::string externalLabel;
  if (!isPageBreakMarker && !idAttr.empty() && self->externalPageBreakAnchorCount > 0) {
    const char* entry = self->externalPageBreakAnchorPool.c_str();
    for (uint16_t i = 0; i < self->externalPageBreakAnchorCount; ++i) {
      const size_t idLen = std::strlen(entry);
      const char* extLabel = entry + idLen + 1;
      const size_t labelLen = std::strlen(extLabel);
      if (idLen == idAttr.size() && std::memcmp(entry, idAttr.data(), idLen) == 0) {
        externalLabel.assign(extLabel, labelLen);
        isPageBreakMarker = true;
        break;
      }
      entry = extLabel + labelLen + 1;
    }
  }

  if (isPageBreakMarker) {
    std::string label = !ariaLabel.empty() ? ariaLabel : titleAttr;
    if (label.empty()) {
      label = std::move(externalLabel);
    }
    self->recordPageBreakLabel(label);
    if (!idAttr.empty()) {
      self->recordAnchor(idAttr, static_cast<uint16_t>(self->completedPageCount));
      self->pendingAnchorId = idAttr;
    }
  }

  // Defer generic anchor recording until startNewTextBlock, after the previous block
  // is flushed to pages via makePages(). Skip pagebreak anchors since they were already recorded.
  //
  // IDs on <span> take their own path (recordSpanAnchor): reading-system converters inject them
  // by the thousand, so they have a budget of their own and never count against
  // MAX_ANCHORS_PER_CHAPTER, which the other elements keep whole -- see its definition. TOC
  // anchors bypass both, since they drive page breaks and core navigation.
  if (!isPageBreakMarker && !idAttr.empty()) {
    const bool isTocAnchor =
        std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idAttr) != self->tocAnchors.end();
    if (isTocAnchor ||
        (!isNonNavigableInlineElement(name) && self->anchorCount < self->anchorLimit() + self->spanAnchorCount_)) {
      self->pendingAnchorId = idAttr;
    } else if (isNonNavigableInlineElement(name)) {
      self->recordSpanAnchor(idAttr);
    } else {
      self->noteCapOverflow(kCapAnchorsPerChapter, "anchors per chapter");
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    {
      // An element with an id is resolved but NOT cached. The key would have to carry the id --
      // the style can legitimately differ per id, that is what a #id selector is for -- and an id
      // is unique in a document, so such an entry can never be hit again. It is pure cost.
      //
      // "ID-bearing elements are uncommon" was the assumption here, and it is wrong of exactly
      // the documents that can least afford it. A converted book's endnotes chapter carries one
      // generated id per note: 349 of them in the chapter this was measured on, each adding a
      // ~40-char key plus a 116-byte CssStyle plus a map node to a cache that is unbounded and
      // never evicted. That is ~70 KB held for the whole build, with a zero percent hit rate,
      // and it was the single largest thing in a build that then ran out of heap and truncated
      // the chapter at page 31 of 90 -- which silently cost the anchor map, and with it every
      // footnote jump into the second half of the chapter.
      //
      // Skipping the cache costs nothing it was buying: unique keys never hit. Elements without
      // an id are unaffected and still share entries by tag|class, which is where the hits are.
      // An id only affects the answer when some rule matches on one. When none does -- neither of
      // this book's stylesheets has a single # selector, nor the other edition's -- the id is
      // dropped from the key entirely and the element shares the tag|class entry, so a chapter of
      // uniquely-id'd notes gets cache HITS instead of a re-resolve (and its SD reads) per note.
      if (!idAttr.empty() && self->cssParser->hasIdSelectors()) {
        cssStyle = self->cssParser->resolveStyle(name, classAttr, idAttr);
      } else {
        std::string cacheKey(name);
        cacheKey += '|';
        cacheKey += classAttr;
        auto it = self->cssStyleCache_.find(cacheKey);
        if (it != self->cssStyleCache_.end()) {
          cssStyle = it->second;
        } else {
          CssStyle resolved = self->cssParser->resolveStyle(name, classAttr, idAttr);
          // An empty result stays out so future calls can re-resolve; anything else is memoised
          // while the memo has room (kStyleMemoMaxEntries). Either way the element uses `resolved`.
          if (resolved.defined.anySet() && self->styleMemoHasRoom(self->cssStyleCache_))
            self->cssStyleCache_.emplace(std::move(cacheKey), resolved);
          cssStyle = resolved;
        }
      }
    }
    if (!styleAttr.empty()) cssStyle.applyOver(self->inlineStyleFor(styleAttr));
  }

  // The HTML `hidden` attribute means display:none, and outranks the CSS that got
  // us here -- it is applied after the cascade above for that reason. EPUBs use it
  // to carry notes, answers and alternate-language blocks that must not be shown.
  // Ported from crosspoint-reader PR #3390 (Joe Harpham / @jjharpham).
  if (hasHiddenAttr) {
    cssStyle.display = CssDisplay::None;
    cssStyle.defined.display = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.). `opacity: 0`
  // and `visibility: hidden` take the same exit: the element and everything in it -- images
  // included -- is invisible, and an invisible block that still took its space would be a
  // blank page on an e-reader, not fidelity.
  if ((cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) || cssStyle.isElementHidden()) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Transparent text is different: the element stays (structure, images, spacing), only its
  // words are dropped, via the same text-only skip the zero-height spacer uses. `<` rather than
  // `=`: a transparent element nested inside a transparent ancestor must not shorten the
  // ancestor's scope, and the reset in endElement only fires at the depth that set it.
  // Known limit of the single slot: a descendant's explicit `color: black` cannot re-enable
  // text inside a transparent ancestor. No OCR layer does that; a stack would if one ever does.
  //
  // Text declared too small to read takes the same exit (isFontSizeInvisible): a Polish watermark
  // is a <span style="color:white; font-size:1px">===LxkoGigb...</span> in the prose, which the
  // MIN_WORD_SIZE_PCT floor raised into visible gibberish. html/body are exempt: a root size is a
  // baseline (the `html { font-size: 6.25% }` 1rem = 1px idiom), not a hidden run. The single
  // slot's limit holds here too -- a descendant re-declaring a readable size stays hidden -- which
  // is why `font-size: 0`, the zero-size container idiom whose children do exactly that, is left
  // alone: it is not parsed as a size at all.
  // Based on crosspoint-reader PR #2326 (Maciej, @mchuck), reworked to hide the text only.
  const bool unreadablySmall = cssStyle.isFontSizeInvisible() && !isRootFontSizeElement(name);
  if ((cssStyle.isTextTransparent() || unreadablySmall) && self->depth < self->skipTextUntilDepth) {
    self->skipTextUntilDepth = self->depth;
  }

  // Background pictures (see BackgroundImageScope): a container with one opens a scope, and inside
  // one a run of <br/> is cut to two -- the rest only made room for the picture behind the text.
  // Skipped like a display:none element, so its close is consumed by the skip path.
  if (strcmp(name, "br") == 0) {
    if (!self->backgroundImageScopes_.empty() && ++self->brRunInBackground_ > 2) {
      self->skipUntilDepth = self->depth;
      self->depth += 1;
      return;
    }
  } else {
    self->brRunInBackground_ = 0;
    self->noteBackgroundImage(name, classAttr, idAttr, styleAttr);
  }

  self->observeFontSizeBaseline(name, cssStyle);
  cssStyle = self->normalizeFontSizeForElement(name, cssStyle);

  // Track an explicit CSS width on a wrapping block (e.g. <div style="width:100px">).
  // A percentage image width inside resolves against the innermost such width, so a
  // width:100% image in a narrow box stays small instead of filling the viewport.
  if (cssStyle.hasImageWidth() && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    const int parentWidth =
        self->containerWidthStack_.empty() ? self->viewportWidth : self->containerWidthStack_.back().width;
    const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    const int w = static_cast<int>(cssStyle.imageWidth.toPixels(emSize, static_cast<float>(parentWidth)) + 0.5f);
    if (w >= 1 && w < parentWidth) {
      const bool relative = cssStyle.imageWidth.unit == CssUnit::Percent;
      const int16_t parentFixed = self->containerWidthStack_.empty() ? 0 : self->containerWidthStack_.back().fixedWidth;
      self->containerWidthStack_.push_back(
          {self->depth, static_cast<int16_t>(w), relative ? parentFixed : static_cast<int16_t>(w)});
    }
  }

  // Streaming table rendering: buffer one row, emit it as part of a PageTableFragment at </tr>.
  if (strcmp(name, "table") == 0) {
    if (self->currentTable) {
      // Nested table — grid layout is now impossible for the outer table, and it cannot recover
      // on the next row either, so the inner table's cells fold into the outer one's stream.
      self->currentTable->depth += 1;
      self->degradeTable("nested table");
      self->depth += 1;
      return;
    }
    // Flush any pending text before starting the table
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    // makePages() empties a block but keeps it, and startNewTextBlock() treats a surviving EMPTY
    // block as a wrapper whose style the next paragraph should inherit (<div style=...><h1>).
    // That is right for a wrapper and wrong for spent remains: <h1>Title</h1><table>… left the
    // heading's 1.6x multiplier standing, so the first block after the table -- a <caption>, or
    // the paragraph following the grid -- merged it and rendered at heading size. Clear the
    // heading sizing rather than destroying the block: text can still arrive before the next
    // block tag opens, and flushPartWordBuffer drops words when there is no block to take them.
    self->clearSpentBlockHeadingStyle();
    self->applyPendingPageBreak();
    self->currentTable = std::unique_ptr<BufferedTable>(new BufferedTable());
    self->currentTable->depth = 1;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "border") == 0 && strcmp(atts[i + 1], "0") == 0) {
          self->currentTable->hasBorder = false;
        }
      }
    }
    // Give the table the box its own CSS asks for. Ported from CrossInk commit 61c8d78f
    // ("feat: improve table rendering (#89)", Julia <julia@uxj.io>), which sizes the grid from
    // the table's block inset; we reuse BlockStyle::fromCssStyle so the 4em inset cap and the
    // em/percentage resolution stay shared with every other block.
    const float tableEmSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    BlockStyle tableBlockStyle = BlockStyle::fromCssStyle(
        cssStyle, tableEmSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);
    self->addAncestorInsets(tableBlockStyle, tableEmSize);
    const int16_t horizontalInset = tableBlockStyle.totalHorizontalInset();
    self->currentTable->contentWidth = (horizontalInset > 0 && horizontalInset < self->viewportWidth)
                                           ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                           : self->viewportWidth;
    self->currentTable->packer.totalWidth = self->currentTable->contentWidth;
    self->currentTable->packer.xInset =
        (self->currentTable->contentWidth < self->viewportWidth) ? tableBlockStyle.leftInset() : 0;
    self->currentTable->packer.hasBorder = self->currentTable->hasBorder;
    self->depth += 1;
    return;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "tr") == 0) {
    // One row of scratch, reused for every <tr>. Clearing rowDegraded here is what bounds a bad
    // row to itself: a row that overflowed the budget, or could not be a grid row, does not stop
    // the next one from going back into the grid.
    auto& t = *self->currentTable;
    t.pendingRow.cells.clear();
    t.pendingRow.effectiveCols = 0;
    t.pendingRow.isHeaderRow = false;
    t.pendingRowBytes = 0;
    t.rowDegraded = false;
    t.rowOverflowed = false;
    self->depth += 1;
    return;
  }

  if (self->currentTable && self->currentTable->depth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    // Gates on the BUFFERING side, so a row that cannot afford grid layout degrades before the
    // memory is committed rather than after. Both are deterministic quantities that accumulate;
    // the heap is NOT sampled here (see heapAllowsTableRowLayout). No cell is open at this
    // point, so degradeRow() is safe here; the mid-cell case goes through degradeRowAtOpenCell()
    // from the word handler.
    if (!self->currentTable->degraded && !self->currentTable->rowDegraded) {
      if (self->currentTable->pendingRowBytes >= MAX_TABLE_ROW_BUFFER_BYTES) {
        self->degradeRow("row size budget");
      } else if (self->currentTable->rowOverflowed) {
        // Deferred from the previous <td>, which was open when the row outgrew MAX_TABLE_COLS.
        self->degradeRow("column overflow");
      }
    }
    // Parse colspan attribute (inspired by uxjulia/CrossInk; rewritten for our codebase).
    // Any rowspan != 1 is unsupported; we ignore it and let the fallback handle those tables.
    // Parsed BEFORE the cell is pushed: a rowspan degrades the table, which clears pendingRow.
    uint8_t colSpan = 1;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "colspan") == 0) {
          char* end;
          const long v = std::strtol(atts[i + 1], &end, 10);
          if (end != atts[i + 1] && v >= 1 && v <= MAX_TABLE_COLS) {
            colSpan = static_cast<uint8_t>(v);
          }
        } else if (strcmp(atts[i], "rowspan") == 0) {
          char* end;
          const long v = std::strtol(atts[i + 1], &end, 10);
          if (end != atts[i + 1] && v != 1) {
            // A rowspan mis-renders every row it covers, not just this one, so it takes the table.
            self->degradeTable("rowspan");
          }
        }
      }
    }

    BufferedTableRow& row = self->currentTable->pendingRow;

    const bool isHeader = (strcmp(name, "th") == 0);
    row.cells.emplace_back();
    row.cells.back().isHeader = isHeader;
    row.cells.back().colSpan = colSpan;
    row.cells.back().text =
        std::unique_ptr<ParsedText>(new ParsedText(false, false));  // no paragraph spacing, no hyphenation in cells
    self->currentTable->pendingRowBytes += TABLE_BUFFER_BYTES_PER_CELL;
    row.effectiveCols = static_cast<uint8_t>(row.effectiveCols + colSpan);
    // Column overflow: this row cannot be a grid row. Not degraded here — the cell just created is
    // still open and about to receive text, and degradeRow() clears the row. Latched for the next
    // <td>, or picked up by commitPendingRow() at </tr> if this was the last cell.
    if (row.cells.size() > MAX_TABLE_COLS || row.effectiveCols > MAX_TABLE_COLS) {
      self->currentTable->rowOverflowed = true;
    }
    self->currentTableCell = &row.cells.back();
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    std::string src;
    std::string alt;
    // Explicit width/height from the markup (e.g. an SVG cover: <image width="455" height="751" .../>,
    // or <img width= height=>). When both are present and numeric they are the intrinsic dimensions the
    // publisher declared, so we can size the image WITHOUT reading the JPEG/PNG header from the ZIP — a
    // ring-hungry inflate that OOMs on the reader's tight heap (the "Failed to read image dimensions:
    // cover.jpeg" that left the titlepage blank). 0 = attribute absent/non-numeric → fall back.
    int attrWidth = 0, attrHeight = 0;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0 || strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0) {
          if (src.empty()) {
            src = atts[i + 1];
            // Strip fragment anchors (e.g. "cover.jpg#xywh=0,0,100,100")
            auto hash = src.find('#');
            if (hash != std::string::npos) src.erase(hash);
          }
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        } else if (strcmp(atts[i], "width") == 0) {
          attrWidth = atoi(atts[i + 1]);  // ignores a trailing "%"/"px"; 0 for non-leading-digit values
        } else if (strcmp(atts[i], "height") == 0) {
          attrHeight = atoi(atts[i + 1]);
        }
      }

      // Image inside a table cell: attach it to the cell so the layout can place it inside the
      // grid (first image per cell wins). The fragment layout sizes the cell to fit it; the
      // paragraph fallback re-emits it as a block image below the table.
      if (self->currentTableCell && self->currentTable && !src.empty() && self->imageRendering != 2) {
        if (self->currentTableCell->imageSrc.empty()) {
          self->currentTableCell->imageSrc = src;
          self->currentTableCell->imageAlt = alt;
        }
        self->depth += 1;
        return;
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        // Suppressing an image should not leak accumulated wrapper block spacing
        // (e.g. figure/h1 margins) into the next text paragraph.
        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
          BlockStyle resetStyle;
          resetStyle.textAlignDefined = true;
          const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(self->paragraphAlignment);
          resetStyle.alignment = align;
          self->currentTextBlock->setBlockStyle(resetStyle);
          LOG_DBG("EHP", "Image suppressed: pending empty block style reset");
        }
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->resolvedImgStyle(classAttr);
        if (!styleAttr.empty()) imgDisplayStyle.applyOver(self->inlineStyleFor(styleAttr));
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          // CSS-hidden images should behave like suppressed images for spacing.
          if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
            BlockStyle resetStyle;
            resetStyle.textAlignDefined = true;
            const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                   ? CssTextAlign::Justify
                                   : static_cast<CssTextAlign>(self->paragraphAlignment);
            resetStyle.alignment = align;
            self->currentTextBlock->setBlockStyle(resetStyle);
            LOG_DBG("EHP", "Image hidden via CSS display:none: pending empty block style reset");
          }
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      const auto handleImageFallback = [&]() {
        // Fallback to alt text if image processing fails.
        if (!alt.empty()) {
          alt = "[Image: " + alt + "]";
          self->startNewTextBlock(centeredBlockStyle);
          self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
          self->depth += 1;
          self->consumeText(userData, alt.c_str(), alt.length(), self->visibleTextOffset_, /*source=*/false);
          // Skip any child content (skip until parent as we pre-advanced depth above)
          self->skipUntilDepth = self->depth - 1;
          return;
        }

        // No alt text, skip.
        self->skipUntilDepth = self->depth;
        self->depth += 1;
      };

      if (!src.empty() && self->imageRendering != 1) {
        LOG_TRC("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (isImageCandidate(resolvedPath)) {
            // Determine SD cache path (image will be extracted here lazily at first render).
            const std::string cachedImagePath = imageCachePathFor(self->imageBasePath, resolvedPath);

            // Get dimensions, cheapest ring-free source first:
            //  1. explicit width/height on the tag (an SVG cover / sized <img>) — no ZIP read at all;
            //  2. the image manifest (each header read at most once ever; a header the probe
            //     window cannot reach is walked only when the heap can host the ring, else deferred
            //     to the build's end — see the Deferred branch);
            //  3. without a manifest, read the ZIP entry header directly — a ~32 KB inflate ring
            //     that can OOM on the reader's tight heap (this is what left the cover.jpeg
            //     titlepage blank).
            ImageDimensions dims = {0, 0};
            bool dimsOk = false;
            bool manifestAnswered = false;
            bool imageDeferred = false;
            // A name without an extension promises nothing: its dimensions must come from its bytes,
            // which is what proves it is an image at all, so the tag's own size cannot stand in.
            if (attrWidth > 0 && attrHeight > 0 && !hasNoFileExtension(resolvedPath)) {
              dims.width = attrWidth;
              dims.height = attrHeight;
              dimsOk = true;
            }
            if (!dimsOk && self->imageManifest) {
              // Resolve + cache on a miss: each image's header is read at most once ever.
              switch (self->imageManifest->resolve(self->epub->getPath(), resolvedPath, dims, self->buildArena_)) {
                case EpubImageManifest::Resolve::Resolved:
                  dimsOk = true;
                  manifestAnswered = true;
                  break;
                case EpubImageManifest::Resolve::Unreadable:
                  // Missing, unknown format or corrupt: alt text for good. The direct read below
                  // would only repeat the same probe on the same bytes.
                  manifestAnswered = true;
                  break;
                case EpubImageManifest::Resolve::Deferred: {
                  // A valid JPEG whose SOF lies beyond the probe window (Exif/IPTC/XMP/ICC ahead
                  // of it — 10-29 KB per image in the books this was measured on). Reaching it
                  // means walking the entry through an inflate ring: 16 KB for the first stage,
                  // the entry's size (≤32 KB) only when the header runs past that. The heap is
                  // asked for exactly the stage the walk takes, within what it can spare now
                  // (imageWalkBudget); a stage that does not fit is latched provisional and left
                  // to the build's end (Epub::persistImageManifest), after which the caller
                  // rebuilds clean. On the reporter's X3 the old entry-sized ring OOMed at page
                  // 54 of a 58-page chapter and the alt text was cached as if the file were
                  // corrupt.
                  using Walk = EpubImageManifest::Walk;
                  // The walk's ring comes from the build arena when it has the room (the entry
                  // reader scopes its block above the page block and releases it before returning),
                  // else from the heap within imageWalkBudget().
                  Walk walk = self->imageManifest->resolveDeferredNow(self->epub->getPath(), resolvedPath, dims,
                                                                      self->imageWalkBudget(), self->buildArena_);
                  if (walk == Walk::NeedsHeap && self->recoverHeapForImageHeader()) {
                    walk = self->imageManifest->resolveDeferredNow(self->epub->getPath(), resolvedPath, dims,
                                                                   self->imageWalkBudget(), self->buildArena_);
                  }
                  if (walk == Walk::Resolved) {
                    dimsOk = true;
                  } else {
                    // The ring did not fit, or the walk found nothing: both are retried at the
                    // build's end, so neither may be cached as final.
                    if (walk == Walk::NeedsHeap) {
                      LOG_DBG("EHP",
                              "Image header walk deferred to build end (%u bytes for its first stage; %u free, %u max "
                              "alloc)",
                              static_cast<unsigned>(self->imageManifest->deferredWalkBytes(resolvedPath)),
                              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
                    }
                    self->imageHeaderSkippedForHeap = true;
                    imageDeferred = true;
                  }
                  manifestAnswered = true;
                  break;
                }
              }
            }
            if (!dimsOk && !manifestAnswered) {
              // Only reached when neither the tag nor the manifest could supply dimensions. On
              // refusal dimsOk stays false and the code below already falls through to
              // handleImageFallback(), so a tight heap degrades exactly this image and no other.
              if (self->heapAllowsImageHeaderRead() ||
                  (self->recoverHeapForImageHeader() && self->heapAllowsImageHeaderRead())) {
                dimsOk = ImageDecoderFactory::getDimensionsFromZipEntry(self->epub->getPath(), resolvedPath, dims);
              } else {
                // Latched, because the two ways of arriving at alt text are not the same thing.
                // An unreadable or unsupported image is alt text for good, and caching that is
                // correct. A heap refusal is a statement about this moment only — the same image
                // resolves fine from a build with more headroom — so the result must not be kept.
                // Background callers discard on this; see Section::isImageHeaderDegraded().
                self->imageHeaderSkippedForHeap = true;
              }
            }
            if (dimsOk) {
              LOG_TRC("EHP", "Image dimensions: %dx%d", dims.width, dims.height);
              {
                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle = self->cssParser ? self->resolvedImgStyle(classAttr) : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty()) imgStyle.applyOver(self->inlineStyleFor(styleAttr));
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();
                // The column the image would get if no percentage wrapper narrowed it.
                int unwrappedWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) unwrappedWidth = self->viewportWidth - inset;
                }
                int containerWidth = unwrappedWidth;
                bool percentWrapper = false;
                if (!self->containerWidthStack_.empty()) {
                  // An ancestor block set an explicit width (e.g. width:100px wrapper);
                  // percentages and fit-to-container both resolve against it.
                  const ContainerWidthEntry& wrapper = self->containerWidthStack_.back();
                  containerWidth = wrapper.width;
                  percentWrapper = wrapper.fixedWidth != wrapper.width;
                  if (wrapper.fixedWidth > 0) unwrappedWidth = wrapper.fixedWidth;
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, fit the image inside that box,
                  // then clamp to the current container.
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  // A browser stretches the image to fill a box of a different ratio. We
                  // deliberately do not: the decoders scale each axis independently and would
                  // honour it, so a stylesheet we only partly understand (no min-/max-width, no
                  // box model) could hand us a box that squashes the picture. Fit inside the
                  // requested box and keep the source ratio.
                  const float boxScale = std::min(static_cast<float>(displayWidth) / dims.width,
                                                  static_cast<float>(displayHeight) / dims.height);
                  displayWidth = std::max(1, static_cast<int>(dims.width * boxScale + 0.5f));
                  displayHeight = std::max(1, static_cast<int>(dims.height * boxScale + 0.5f));
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_TRC("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_TRC("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive
                  // height from aspect ratio.
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_TRC("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit current container while maintaining aspect ratio.
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_TRC("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // A percentage wrapper (<div class="full60">) is layout for a large screen, where
                // 60% of the column still leaves a picture near its native resolution. On a
                // 480px column it turns a riddle diagram into a thumbnail, so it may enlarge an
                // image but never shrink one below min(native, unwrapped column). Only images
                // sized by the container count: an explicit px/em size or a CSS height is the
                // publisher sizing the image itself and stays as resolved above. A float is left
                // alone too: its percentage width is what leaves room for the text beside it.
                const bool sizedByContainer =
                    !hasCssHeight && (!hasCssWidth || imgStyle.imageWidth.unit == CssUnit::Percent);
                if (percentWrapper && sizedByContainer && self->floatDepth_ == 0 && dims.width > 0 && dims.height > 0) {
                  int floorWidth =
                      hasCssWidth ? static_cast<int>(
                                        imgStyle.imageWidth.toPixels(emSize, static_cast<float>(unwrappedWidth)) + 0.5f)
                                  : unwrappedWidth;
                  floorWidth = std::min({floorWidth, unwrappedWidth, static_cast<int>(dims.width)});
                  int floorHeight = static_cast<int>(static_cast<int64_t>(floorWidth) * dims.height / dims.width);
                  if (floorHeight > self->viewportHeight) {
                    floorHeight = self->viewportHeight;
                    floorWidth = static_cast<int>(static_cast<int64_t>(floorHeight) * dims.width / dims.height);
                  }
                  if (floorWidth > displayWidth) {
                    LOG_TRC("EHP", "Percent wrapper floor: %dx%d -> %dx%d", displayWidth, displayHeight, floorWidth,
                            floorHeight);
                    displayWidth = std::max(1, floorWidth);
                    displayHeight = std::max(1, floorHeight);
                  }
                }

                // Inline image path: if inside a CSS float context and the image leaves a
                // usable text column, defer placement beside the following paragraph rather
                // than emitting it as a centered block.
                // Concept inspired by CidVonHighwind/microreader and KOReader/CREngine research.
                //
                // Width: the float must leave at least half the column for text, otherwise the
                //   wrapped lines are too narrow to read — fall back to a centered block.
                // Height: a real figright/figleft illustration (e.g. a half-page Gutenberg plate)
                //   is much taller than a drop-cap or decorator, so allow up to a full viewport.
                //   attachPendingFloatImage() splits anything taller than the remaining page into
                //   a continuation tile on the next page; capping at viewportHeight keeps that
                //   single-continuation split correct (tileB never exceeds one page).
                const bool isInlineCandidate = self->floatDepth_ > 0 && displayWidth <= self->viewportWidth / 2 &&
                                               displayHeight <= self->viewportHeight;
                if (isInlineCandidate) {
                  self->pendingInlineImage_.cachedPath = std::move(cachedImagePath);
                  self->pendingInlineImage_.epubEntryPath = resolvedPath;
                  self->pendingInlineImage_.width = static_cast<int16_t>(displayWidth);
                  self->pendingInlineImage_.height = static_cast<int16_t>(displayHeight);
                  self->pendingInlineImage_.alt = alt;
                  self->pendingInlineImage_.isRight =
                      (self->floatDepth_ > 0) && self->floatOpenSides_[self->floatDepth_ - 1];
                  self->pendingInlineImage_.active = true;
                  LOG_TRC("EHP", "Inline image deferred: w=%d h=%d", displayWidth, displayHeight);
                  // Don't flush the current text block — let it continue into the next paragraph.
                  self->depth += 1;
                  return;
                }

                // Block image path (existing behaviour) — flush text before placing image
                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  if (!self->flushPartWordBuffer()) return;
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }
                self->applyPendingPageBreak();

                // If the current text block is still empty, it may carry accumulated parent
                // block spacing (e.g. div/figure/h1 wrappers). Apply that spacing around the
                // image itself so it doesn't leak into the next text paragraph.
                BlockStyle pendingImageBlockStyle;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  pendingImageBlockStyle = self->currentTextBlock->getBlockStyle();
                }

                const int imageSpacingTop = std::max(0, static_cast<int>(pendingImageBlockStyle.marginTop)) +
                                            std::max(0, static_cast<int>(pendingImageBlockStyle.paddingTop));
                const int imageSpacingBottom = std::max(0, static_cast<int>(pendingImageBlockStyle.marginBottom)) +
                                               std::max(0, static_cast<int>(pendingImageBlockStyle.paddingBottom));
                const int totalImageHeightWithSpacing = imageSpacingTop + displayHeight + imageSpacingBottom;

                LOG_DBG("EHP",
                        "Image layout prep: src=%s dims=%dx%d display=%dx%d y=%d spacing(top=%d,bottom=%d,total=%d)",
                        src.c_str(), dims.width, dims.height, displayWidth, displayHeight, self->currentPageNextY,
                        imageSpacingTop, imageSpacingBottom, totalImageHeightWithSpacing);

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + totalImageHeightWithSpacing > self->viewportHeight)) {
                  LOG_TRC("EHP", "Image page break: currentY=%d needed=%d viewportH=%d", self->currentPageNextY,
                          totalImageHeightWithSpacing, self->viewportHeight);
                  self->emitPage();
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                self->currentPageNextY += imageSpacingTop;

                // Create ImageBlock with lazy-extraction source info.
                // The SD file at cachedImagePath does not exist yet — it will be extracted
                // from the EPUB at first render time by ImageBlock::ensureExtracted().
                auto imageBlock = makeUniqueNoThrow<ImageBlock>(cachedImagePath, displayWidth, displayHeight, alt,
                                                                self->epub->getPath(), resolvedPath);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = makeUniqueNoThrow<PageImage>(std::move(imageBlock), xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->noteElementOnPage(self->visibleTextOffset_);
                self->currentPage->elements.push_back(std::move(pageImage));
                self->currentPageNextY += displayHeight;
                self->currentPageNextY += imageSpacingBottom;

                LOG_TRC("EHP", "Image placed: x=%d y=%d w=%d h=%d nextY=%d", xPos, pageImage->yPos, displayWidth,
                        displayHeight, self->currentPageNextY);

                // Reset empty pending block style after consuming spacing around the image.
                // This prevents figure/header wrapper margins from being applied again to the
                // next paragraph block.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.textAlignDefined = true;
                  const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                         ? CssTextAlign::Justify
                                         : static_cast<CssTextAlign>(self->paragraphAlignment);
                  resetStyle.alignment = align;
                  self->currentTextBlock->setBlockStyle(resetStyle);
                  LOG_TRC("EHP", "Image spacing consumed; pending empty block style reset for following text");
                }

                self->depth += 1;
                return;
              }  // layout geometry block
            } else if (imageDeferred) {
              LOG_DBG("EHP", "Image header deferred to the build's end: %s", resolvedPath.c_str());
            } else {
              LOG_ERR("EHP", "Failed to read image dimensions from ZIP: %s", resolvedPath.c_str());
            }
          }  // isImageCandidate
        }
      }

      handleImageFallback();
      return;
    }
  }

  // Track body element depth for paragraph index counting and for the visible-text count, which
  // starts inside it. Case-insensitive: the KOReader mappers lowercase tag names, so text under a
  // <BODY> is source text to them and must be here too.
  if (strcasecmp(name, "body") == 0 && self->xpathBodyDepth < 0) {
    self->xpathBodyDepth = self->depth;
  }

  // Count <p> sibling indices at body-child level. Must happen BEFORE the display:none
  // check so that hidden <p> elements are still counted, matching ChapterXPathIndexer's
  // counting (pure XML, no CSS). This ensures paragraph indices in the section cache LUT
  // align with KOReader's crengine XPath indices.
  if (self->xpathBodyDepth >= 0 && self->depth == self->xpathBodyDepth + 1) {
    if (strcmp(name, "p") == 0) {
      self->xpathParagraphIndex++;
    }
  }

  // <li> can appear nested inside <ul>/<ol> at any depth, so count it globally —
  // not at body-child level. The running count must match what the runtime reverse
  // mapper sees so getPageForListItemIndex can snap a KOReader li XPath to a page.
  if (self->xpathBodyDepth >= 0 && strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // removed skipping of doc-pagebreak and epub:type="pagebreak"
  // as publishers sometimes wrap actual content in these tags
  /*
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }
  */

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
        if (!self->flushPartWordBuffer()) return;
        if (!endsAtDashBreak) {
          self->nextWordContinues = true;
        }
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      if (strlen(href) >= sizeof(self->currentFootnote.href)) {
        // A truncated href would be navigated as-is and land nowhere. Keep the link's styling,
        // record no footnote for it (R4: refuse the entry rather than store a broken one).
        self->currentFootnote.href[0] = '\0';
        self->noteCapOverflow(kCapFootnoteHref, "footnote href length");
      } else {
        strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
        self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      }
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      // The generic inline path below never runs for an internal link, so vertical-align has to be
      // folded in here or it is lost. Publishers mark footnote references as
      // `a { vertical-align: super }` at least as often as they wrap them in <sup>, and those
      // references were rendering full-size on the baseline.
      // Ported from crosspoint-reader PR #3355 (Julia <julia@uxj.io>). Their fix extracts the same
      // helper; ours additionally honours `baseline` as an explicit cancel, which our inline path
      // already did and theirs does not.
      applyVerticalAlignToEntry(entry, cssStyle);
      // Not upstream's: they raise the reference but leave it full-size, which does not match the
      // <sup> path three branches down. These two are documented as a pair -- the default first so
      // publisher CSS on the link (`a.fn { font-size: 0.7em }`) still wins over the 50% default.
      // applyCssFontSizeToEntry is a no-op unless the link itself carries a font-size.
      applySupSubDefaultSize(entry);
      applyCssFontSizeToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip the rest of CSS resolution — the styling this <a> needs is applied above
      self->depth += 1;
      return;
    }
  }

  // Track CSS float depth — used to detect inline images beside paragraph text.
  // Fixed-size array, cap at kMaxFloatDepth — deeper nesting is pathological.
  const bool isFloated = cssStyle.hasCssFloat() && cssStyle.cssFloat != CssFloat::None;
  if (isFloated && self->floatDepth_ < ChapterHtmlSlimParser::kMaxFloatDepth) {
    self->floatOpenDepths_[self->floatDepth_] = self->depth;
    self->floatOpenSides_[self->floatDepth_] = (cssStyle.cssFloat == CssFloat::Right);
    self->floatDepth_++;
  }

  const bool isList = strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0;
  if (isList) {
    int startCounter = 0;
    if (name[0] == 'o') {
      const char* startAttr = getAttribute(atts, "start");
      if (startAttr) {
        int v = atoi(startAttr);
        if (v > 0) startCounter = v - 1;  // counter is pre-incremented on each <li>
      }
    }
    self->listStack.push_back({self->depth, name[0] == 'o', startCounter, cssStyle.listStyleNone});
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);
  // As in CSS, a list's own padding-left replaces the default and its margin-left adds to it.
  if (isList && !cssStyle.hasPaddingLeft()) {
    userAlignmentBlockStyle.paddingLeft = static_cast<int16_t>(emSize * LIST_DEFAULT_PADDING_EM);
  }

  // This element's own inset is what its children inherit; capture it before the ancestors are
  // folded in, or each level would count itself once per descendant.
  const int16_t ownInsetLeft = userAlignmentBlockStyle.leftInset();
  const int16_t ownInsetRight = userAlignmentBlockStyle.rightInset();
  self->addAncestorInsets(userAlignmentBlockStyle, emSize);

  // Containers that are not BLOCK_TAGS (<ul>, <ol>, <section>, <article>, <aside>, <main>) are still
  // blocks with vertical spacing of their own. Each starts an empty block holding its top spacing,
  // which its first child merges into (collapsing with the child's margin-top, as under a wrapper
  // <div>); its bottom spacing is held for the end tag like any block element's. The block's
  // horizontal inset is the enclosing one, not the container's (pushed below, for the children):
  // a list marker still waiting from an enclosing <li> lands in it. Table cells lay their text out
  // apart from the page's blocks, so a container inside one is left alone.
  const bool isBlockContainer = matches(name, INSET_CONTAINER_TAGS, NUM_INSET_CONTAINER_TAGS) && !isHeaderOrBlock(name);
  if (isBlockContainer && !self->currentTableCell) {
    if (self->partWordBufferIndex > 0 && !self->flushPartWordBuffer()) return;
    BlockStyle containerTop = BlockStyle::fromCssStyle(
        CssStyle{}, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);
    containerTop.marginTop = userAlignmentBlockStyle.marginTop;
    containerTop.paddingTop = userAlignmentBlockStyle.paddingTop;
    self->addAncestorInsets(containerTop, emSize);
    self->startNewTextBlock(containerTop);
    self->holdBottomSpacing(userAlignmentBlockStyle, isFloated);
  }

  if ((ownInsetLeft > 0 || ownInsetRight > 0) &&
      self->blockInsetStack_.size() < ChapterHtmlSlimParser::kMaxBlockInsetDepth &&
      matches(name, INSET_CONTAINER_TAGS, NUM_INSET_CONTAINER_TAGS)) {
    self->blockInsetStack_.push_back({self->depth, ownInsetLeft, ownInsetRight});
  }

  // A <span> the book styles display:block is laid out as a block (#388): St. Martin's chapter
  // heads are two of them in one <h1>, the number above the title. Not when floated (out of flow,
  // and a floated span is a drop cap), not inside a table (cells lay their text out apart from
  // the page's blocks), and not inside another such span.
  const bool opensBlockSpan = strcmp(name, "span") == 0 && cssStyle.hasDisplay() &&
                              cssStyle.display == CssDisplay::Block && !isFloated && !self->currentTable &&
                              self->blockSpan_.depth == INT_MAX;

  // Block/header boundaries must flush any buffered trailing word first.
  // Otherwise tags like ..."item?"<p ...> can carry the final word into the next paragraph.
  if (self->partWordBufferIndex > 0 &&
      ((matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) ||
       (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) && strcmp(name, "br") != 0) || opensBlockSpan)) {
    if (!self->flushPartWordBuffer()) return;
  }

  // CSS page-break-before / page-break-after: always. The break before waits for the block's
  // startNewTextBlock, which lays out the paragraph still pending before it: taken here, the page
  // was emitted first and that paragraph went over with this block. The break after is taken at
  // the first content after the end tag. Not inside a table, whose rows are placed as a whole.
  const bool breakableBlock =
      matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || opensBlockSpan;
  if (breakableBlock && !self->currentTable) {
    if (cssStyle.pageBreakBefore) self->pageBreakPending_ = true;
    if (cssStyle.pageBreakAfter && self->breakAfterCount_ < ChapterHtmlSlimParser::kMaxBreakAfterDepth) {
      self->breakAfterDepths_[self->breakAfterCount_++] = self->depth;
    }
  }

  if (opensBlockSpan) {
    self->startBlockSpan(cssStyle, userAlignmentBlockStyle);
    // Its size and margins went to its block. What the span branch below still pushes as an
    // inline entry is its weight, style and decoration: the parts that end with the span.
    cssStyle.defined.fontSizeMultiplier = 0;
    cssStyle.defined.marginLeft = 0;
    cssStyle.defined.verticalAlign = 0;
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->pushBlockStyleEntry(cssStyle);
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    self->addAncestorInsets(headerBlockStyle, emSize);
    headerBlockStyle.textAlignDefined = true;
    headerBlockStyle.keepWithNext = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign() &&
        self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    // Apply default heading sizing when no explicit CSS font-size is set.
    // Concept inspired by CidVonHighwind/microreader. h1-h3 get default multipliers;
    // h4-h6 stay at 1.0. The multiplier (default or CSS) is snapped to the size ladder
    // by resolveBlockFont() once the block is complete, so headings render with a real
    // taller font when one exists and only the residual is glyph-scaled.
    if (!cssStyle.hasFontSizeMultiplier()) {
      const int level = name[1] - '0';  // 'h1'->1, 'h2'->2, …
      if (level >= 1 && level <= 3) {
        headerBlockStyle.fontSizeMultiplier = kHeadingMultiplier[level - 1];
      }
    }
    if (self->headingDepth_ == INT_MAX) {
      self->headingDepth_ = self->depth;
      self->headingAlignment_ = headerBlockStyle.alignment;
      self->headingFontSizeMultiplier_ = headerBlockStyle.fontSizeMultiplier;
    }
    self->holdBottomSpacing(headerBlockStyle, isFloated);
    self->startNewTextBlock(headerBlockStyle);
    self->currentBlockOwnerDepth_ = self->depth;
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (isZeroHeightSpacerParagraph(name, styleAttr)) {
      // Preserve paragraph break semantics for this <p>, but skip its inner text payload.
      self->pushBlockStyleEntry(cssStyle);
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign() &&
          self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      self->holdBottomSpacing(blockStyle, isFloated);
      self->startNewTextBlock(blockStyle);
      self->currentBlockOwnerDepth_ = self->depth;
      self->updateEffectiveInlineStyle();

      // `<`, not `=`: inside a transparent-text ancestor the slot already holds a shallower
      // depth, and overwriting it would end the ancestor's scope when this spacer closes.
      if (self->depth < self->skipTextUntilDepth) self->skipTextUntilDepth = self->depth;
      self->depth += 1;
      return;
    }

    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        if (!self->flushPartWordBuffer()) return;
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      // Build a neutral <br> style that keeps inline alignment/indent context but avoids
      // carrying cumulative margins from previous empty blocks (which can force spurious page breaks).
      const BlockStyle& currentStyle = self->currentTextBlock->getBlockStyle();
      BlockStyle brStyle;
      brStyle.alignment = currentStyle.alignment;
      brStyle.textAlignDefined = currentStyle.textAlignDefined;
      // The horizontal inset is the enclosing block's, not the <br>'s: a <br> splits one
      // paragraph into several blocks, and all of them sit inside the same containing block.
      // (Vertical margins are deliberately not carried — they would repeat per line.)
      self->addAncestorInsets(brStyle, emSize);
      // text-indent is not inherited across <br>: it applies to the first line of a block only.
      // Span-based indents (poem stanza pattern) are applied directly to each block at span-open time.
      // The font size is: the next line belongs to the same heading or paragraph. All three fields
      // travel together -- the block may already be resolved (a long block is laid out in parts),
      // and resolveBlockFont leaves a resolved style alone.
      if (self->currentBlockOwnerDepth_ >= 0) {
        brStyle.fontSizeMultiplier = currentStyle.fontSizeMultiplier;
        brStyle.headingFontId = currentStyle.headingFontId;
        brStyle.fontResolved = currentStyle.fontResolved;
        brStyle.keepWithNext = currentStyle.keepWithNext;
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->pushBlockStyleEntry(cssStyle);
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign() &&
          self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      if (self->depth > self->headingDepth_) self->applyHeadingScope(blockStyle, cssStyle);
      self->holdBottomSpacing(blockStyle, isFloated);
      self->startNewTextBlock(blockStyle);
      self->currentBlockOwnerDepth_ = self->depth;
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        if (!self->listStack.empty()) {
          if (self->listStack.back().isOrdered) {
            const char* valueAttr = getAttribute(atts, "value");
            if (valueAttr) {
              int v = atoi(valueAttr);
              if (v > 0) self->listStack.back().counter = v - 1;
            }
            self->listStack.back().counter += 1;
          }
          if (!self->listStack.back().suppressMarker) {
            char* marker = self->pendingListMarker_;
            if (self->listStack.back().isOrdered) {
              snprintf(marker, sizeof(self->pendingListMarker_), "%d.", self->listStack.back().counter);
            } else {
              strcpy(marker, "\xe2\x80\xa2");
            }
          }
        }
      } else if (strcmp(name, "pre") == 0) {
        // Record depth so characterData can treat \n as a hard line break inside <pre>.
        // depth has not been incremented yet here; it will be after startElement returns.
        self->preUntilDepth = std::min(self->preUntilDepth, self->depth);
      }
    }
  } else if (strcmp(name, "hr") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    self->makePages();
    self->applyPendingPageBreak();
    if (!self->currentPage) {
      self->currentPage.reset(new Page());
      self->currentPageNextY = 0;
    }
    const int lineHeight = static_cast<int>(self->renderer.getLineHeight(self->fontId) * self->lineCompression + 0.5f);
    const int16_t marginV = static_cast<int16_t>(lineHeight / 2);
    self->currentPageNextY += marginV;
    if (self->currentPageNextY + 1 + marginV > self->viewportHeight) {
      self->emitPage();
      self->currentPage.reset(new Page());
      self->currentPageNextY = 0;
    }
    // Render the rule centered at 50% width (25%→75%) rather than edge-to-edge, matching
    // the conventional reader default. Books rarely set hr width in their own CSS.
    const int16_t hrWidth = static_cast<int16_t>(self->viewportWidth / 2);
    const int16_t hrX = static_cast<int16_t>(self->viewportWidth / 4);
    if (auto hr = makeUniqueNoThrow<PageHR>(hrX, self->currentPageNextY, hrWidth)) {
      self->noteElementOnPage(self->visibleTextOffset_);
      self->currentPage->elements.push_back(std::move(hr));
    }
    self->currentPageNextY += 1 + marginV;
    BlockStyle emptyStyle;
    self->startNewTextBlock(emptyStyle);
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) ||
             matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    }
    if (matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
      self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
    }
    // Push inline style entry for underline/strikethrough tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
      entry.hasUnderline = true;
      entry.underline = true;
    }
    if (matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
      entry.hasStrikethrough = true;
      entry.strikethrough = true;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
        self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
        self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
      }
    }
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    applyCssFontSizeToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
      }
    }
    applyCssFontSizeToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
      }
    }
    applyCssFontSizeToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    applySupSubDefaultSize(entry);
    applyCssFontSizeToEntry(entry, cssStyle);  // explicit CSS font-size overrides the 50% default
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Drop cap: a left-floated span with a large font-size opening an empty paragraph.
    // Capture its text for float-zone rendering instead of pushing an inline entry
    // (the per-word size channel would clamp the cap to 250%).
    if (self->tryStartDropCapCapture(cssStyle)) {
      self->depth += 1;
      return;
    }
    // Handle span and other inline elements for CSS styling.
    // <small>/<big> carry UA-default sizes (80%/120%) even without any CSS rule.
    const bool isSmallTag = strcmp(name, "small") == 0;
    const bool isBigTag = strcmp(name, "big") == 0;
    // Block containers that are not BLOCK_TAGS (<ul>, <blockquote>, <section>, ...) land here too,
    // for their font properties. Their margin-left is an inset (blockInsetStack_), not a per-line
    // indent: taken as one it overwrote the text-indent of the paragraph still open before them.
    const bool spanIndent = cssStyle.hasMarginLeft() && !matches(name, INSET_CONTAINER_TAGS, NUM_INSET_CONTAINER_TAGS);
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasVerticalAlign() || cssStyle.hasSmallCaps() || spanIndent || cssStyle.hasFontSizeMultiplier() ||
        isSmallTag || isBigTag) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
        if (!self->flushPartWordBuffer()) return;
        if (!endsAtDashBreak) {
          self->nextWordContinues = true;
        }
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      applyCssEmphasisToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      if (spanIndent) {
        // margin-left on an inline span acts as a per-line indent (poem stanza pattern).
        // Applied immediately to the current block because the span closes before the
        // trailing <br>, so the indent must be on the block that receives the text.
        const int16_t marginPx = cssStyle.marginLeft.toPixelsInt16(emSize, static_cast<float>(self->viewportWidth));
        entry.hasMarginLeft = true;
        entry.marginLeftPx = marginPx;
        if (marginPx > 0 && self->currentTextBlock) {
          BlockStyle updatedStyle = self->currentTextBlock->getBlockStyle();
          updatedStyle.textIndent = marginPx;
          updatedStyle.textIndentDefined = true;
          updatedStyle.textIndentYields = true;
          self->currentTextBlock->setBlockStyle(updatedStyle);
        }
      }
      applySupSubDefaultSize(entry);  // vertical-align: super/sub spans get the 50% default
      applyCssFontSizeToEntry(entry, cssStyle);
      if (!entry.hasFontSize && (isSmallTag || isBigTag)) {
        entry.hasFontSize = true;
        entry.fontSizePct = isSmallTag ? 80 : 120;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void ChapterHtmlSlimParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  const bool source = self->xpathBodyDepth >= 0 && self->nonVisibleTextDepth_ == 0;
  const uint32_t base = self->visibleTextOffset_;
  if (source) {
    self->visibleTextOffset_ += static_cast<uint32_t>(VisibleText::visibleBytes(s, len));
  }
  consumeText(userData, s, len, base, source);
}

void ChapterHtmlSlimParser::consumeText(void* userData, const char* s, const int len, const uint32_t baseOffset,
                                        const bool source) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  // Running offset of s[i]; advances only for source text, so synthetic words sit at baseOffset.
  uint32_t off = baseOffset;

  if (self->streamFailed) {
    return;
  }

  // Skip content of nested tables (depth > 1 means we're inside a nested table)
  if (self->currentTable && self->currentTable->depth > 1) {
    return;
  }

  // Route character data into the active table cell's ParsedText
  if (self->currentTableCell) {
    // Use the existing partWordBuffer + word-level accumulation logic below,
    // but the flush target will be currentTableCell->text (handled in flushPartWordBuffer).
    // Fall through to the normal character accumulation path.
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Text ends a run of <br/> inside a background scope; the whitespace between them does not.
  if (self->brRunInBackground_ != 0) {
    for (int i = 0; i < len; ++i) {
      if (!isWhitespace(s[i])) {
        self->brRunInBackground_ = 0;
        break;
      }
    }
  }

  // Ignore character data inside synthetic zero-height spacer <p> tags.
  if (self->skipTextUntilDepth < self->depth) {
    return;
  }

  // Skip SVG text content (path data, coordinates, etc.) — it would be treated as words
  // and exhaust heap on EPUBs with large inline SVG elements.
  if (self->svgDepth > 0) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Remove leading/trailing whitespace and square brackets from the
  // footnote link text to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  // Route drop-cap span text into the capture buffer instead of the word flow.
  if (self->pendingDropCap_.active) {
    int i = 0;
    bool overflow = false;
    uint32_t byteOff = off;
    for (; i < len; i++) {
      byteOff = off;
      if (source && !VisibleText::isSpace(static_cast<unsigned char>(s[i]))) off++;
      if (isWhitespace(s[i])) continue;
      // Same reason as in the word flow below: a formatting control must not become the
      // drop cap. A watermarked chapter can open with a run of them.
      if (const int ignorableLen = defaultIgnorableLen(s, i, len); ignorableLen > 0) {
        if (source) off += ignorableLen - 1;
        i += ignorableLen - 1;
        continue;
      }
      if (self->pendingDropCap_.textLen >= static_cast<int>(sizeof(self->pendingDropCap_.text)) - 1) {
        overflow = true;
        break;
      }
      if (self->pendingDropCap_.textLen == 0) self->pendingDropCap_.visibleOffset = byteOff;
      self->pendingDropCap_.text[self->pendingDropCap_.textLen++] = s[i];
    }
    if (!overflow) return;
    // Too much text for a drop cap — abandon capture and reroute everything captured
    // so far (plus the rest of this chunk) through the normal inline word flow.
    self->pendingDropCap_.active = false;
    const int captured = self->pendingDropCap_.textLen;
    self->pendingDropCap_.textLen = 0;
    consumeText(userData, self->pendingDropCap_.text, captured, self->pendingDropCap_.visibleOffset, source);
    consumeText(userData, s + i, len - i, byteOff, source);
    return;
  }

  for (int i = 0; i < len; i++) {
    const uint32_t byteOff = off;
    if (source && !VisibleText::isSpace(static_cast<unsigned char>(s[i]))) off++;
    const unsigned char c = static_cast<unsigned char>(s[i]);

    // Fast path for plain ASCII word characters (> 0x20 and < 0x80).
    // This covers the vast majority of characters in Latin-script text.
    // All multi-byte UTF-8 sequences start with a byte >= 0x80, so this
    // path is safe to take without any further multi-byte checks.
    if (c > 0x20 && c < 0x80) {
      if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
        // Buffer is full — flush before appending. Pure ASCII means no
        // partial multi-byte sequence can be at the boundary.
        if (!self->flushPartWordBuffer()) return;
      }
      if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      continue;
    }

    if (isWhitespace(s[i])) {
      // Inside <pre>: treat \n as a hard line break.
      if (s[i] == '\n' && self->preUntilDepth < self->depth) {
        if (self->partWordBufferIndex > 0) {
          if (!self->flushPartWordBuffer()) return;
        }
        // Blank line: the current block is empty, but we still need to emit a visible
        // empty line.  Add a single space so the block is non-empty and makePages()
        // will produce a line of the correct height instead of reusing the empty block.
        if (self->currentTextBlock->isEmpty()) {
          self->currentTextBlock->addWord(" ", EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT,
                                          off);
        }
        // A float the new block takes stands before the next byte of this chunk.
        const PinnedPlacement atNextByte(*self, off);
        self->startNewTextBlock(self->currentTextBlock->getBlockStyle());
        self->nextWordContinues = false;
        continue;
      }
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Drop Default_Ignorable formatting controls before they can become text.
    //
    // Unlike the no-break spaces below these produce NO token at all: they are not word
    // boundaries (so the surrounding text stays one word — "wor<ZWJ>d" is "word"), they carry no
    // width, and they must not reach the renderer. See utf8IsDefaultIgnorable for why this is
    // load-bearing rather than cosmetic on watermarked books.
    //
    // NOTE: this also drops U+200B ZERO WIDTH SPACE, which in correct typography is a break
    // OPPORTUNITY. That is not a regression — isWhitespace() above is ASCII-only, so U+200B has
    // never been a break opportunity in this parser. Making it one is a separate improvement.
    if (const int ignorableLen = defaultIgnorableLen(s, i, len); ignorableLen > 0) {
      i += ignorableLen - 1;  // loop's ++i consumes the last byte
      if (source) off += ignorableLen - 1;
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset_ = byteOff;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      if (!self->flushPartWordBuffer()) return;

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      if (source) off++;
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordVisibleOffset_ = byteOff;
      self->nextWordContinues = true;
      if (!self->flushPartWordBuffer()) return;

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      if (source) off += 2;
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const char FEFF_BYTE_1 = static_cast<char>(0xEF);
    const char FEFF_BYTE_2 = static_cast<char>(0xBB);
    const char FEFF_BYTE_3 = static_cast<char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;  // Skip the next two bytes
        if (source) off += 2;
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        if (!self->flushPartWordBuffer()) return;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        // The saved bytes were counted at their own positions; synthetic text never advanced.
        self->partWordVisibleOffset_ = source ? byteOff - static_cast<uint32_t>(overflow) : byteOff;
      } else {
        if (!self->flushPartWordBuffer()) return;
      }
    }

    if (self->partWordBufferIndex == 0) self->partWordVisibleOffset_ = byteOff;
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }
}

void ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void ChapterHtmlSlimParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  endElementBody(self, name);
  // A background picture goes in once its element has closed: after the element's own text and,
  // for a table, after the grid </table> has just emitted. Out here because the body returns from
  // a dozen places.
  while (!self->streamFailed && !self->backgroundImageScopes_.empty() &&
         self->backgroundImageScopes_.back().depth >= self->depth) {
    const std::string path = std::move(self->backgroundImageScopes_.back().path);
    self->backgroundImageScopes_.pop_back();
    self->brRunInBackground_ = 0;
    self->placeBackgroundImage(path);
  }
}

void ChapterHtmlSlimParser::endElementBody(ChapterHtmlSlimParser* self, const char* name) {
  if (VisibleText::isNonVisibleTag(name) && self->nonVisibleTextDepth_ > 0) self->nonVisibleTextDepth_--;
  if (self->streamFailed) {
    return;
  }

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;
  const bool willClearStrikethrough = self->strikethroughUntilDepth == self->depth - 1;

  const bool styleWillChange =
      willPopStyleStack || willClearBold || willClearItalic || willClearUnderline || willClearStrikethrough;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);
  const bool closesBlockSpan = self->blockSpan_.depth == self->depth - 1;

  if (self->currentTable && self->currentTable->depth > 1 && strcmp(name, "table") == 0) {
    self->partWordBufferIndex = 0;
    self->currentTable->depth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table end, depth now %d", self->currentTable->depth);
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !closesBlockSpan && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || closesBlockSpan ||
                             matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) ||
                             matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      // If closing an inline element, the next word fragment continues the same visual word —
      // unless the buffered text ended at a dash that should allow a line break (em/en dash, etc.).
      if (isInlineTag && !endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
  }

  // An item that closes without text (<li></li>, or one holding only hidden content) still shows
  // its marker, and a marker never outlives its item into the text after the list.
  if (strcmp(name, "li") == 0 || strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
    self->emitPendingListMarker();
  }

  self->depth -= 1;

  // Decrement float depth when the floated element's scope closes.
  while (self->floatDepth_ > 0 && self->floatOpenDepths_[self->floatDepth_ - 1] >= self->depth) {
    self->floatDepth_--;
  }

  // Closing the drop-cap span — place the captured letter and its float zone.
  if (self->pendingDropCap_.active && self->pendingDropCap_.depth == self->depth) {
    self->finalizePendingDropCap();
  }

  if (strcmp(name, "svg") == 0 && self->svgDepth > 0) {
    self->svgDepth -= 1;
  }

  // Pop list entries whose ul/ol is now out of scope
  while (!self->listStack.empty() && self->listStack.back().depth >= self->depth) {
    self->listStack.pop_back();
  }

  // The element that set up the current block has closed. If nothing reached that block (an empty
  // heading, a trailing <br>), it will merge into the next sibling, which must not inherit the size.
  if (self->depth == self->currentBlockOwnerDepth_) {
    self->currentBlockOwnerDepth_ = -1;
    self->clearSpentBlockHeadingStyle();
  }
  if (self->depth == self->headingDepth_) self->headingDepth_ = INT_MAX;

  // Apply held bottom spacing whose block-level element is now out of scope
  while (!self->heldBottomSpacing_.empty() && self->heldBottomSpacing_.back().depth >= self->depth) {
    self->applyHeldBottomSpacing(self->heldBottomSpacing_.back());
    self->heldBottomSpacing_.pop_back();
  }

  // An element that breaks after itself has closed: what follows starts the next page.
  while (self->breakAfterCount_ > 0 && self->breakAfterDepths_[self->breakAfterCount_ - 1] >= self->depth) {
    self->breakAfterCount_--;
    self->pageBreakPending_ = true;
  }

  // Pop explicit-width container entries whose block is now out of scope
  while (!self->containerWidthStack_.empty() && self->containerWidthStack_.back().depth >= self->depth) {
    self->containerWidthStack_.pop_back();
  }

  // Pop horizontal insets whose block-level element is now out of scope
  while (!self->blockInsetStack_.empty() && self->blockInsetStack_.back().depth >= self->depth) {
    self->blockInsetStack_.pop_back();
  }

  // After its bottom spacing has landed on its block, so the next block starts clean.
  if (closesBlockSpan) self->endBlockSpan();

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry = self->currentFootnote;
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    if (self->inlineFootnotePreviews && self->currentFootnote.href[0] != '\0') {
      // Membership in the preview store is the gate: only targets of footnote-shaped links
      // are ever resolved into it, so any resolving href — same-file or
      // cross-file ("../Text/notes.xhtml#n3", Calibre filepos anchors) — is a real note.
      std::string preview;
      if (self->inlineFootnotePreviews->find(self->currentFootnote.href, preview)) {
        self->pendingInlineFootnotePreview = self->abbreviateInlineFootnote(preview.c_str());
        if (!self->pendingInlineFootnotePreview.empty()) {
          LOG_DBG("EHP", "Expanded inline footnote: href=%s previewBytes=%u", self->currentFootnote.href,
                  static_cast<uint32_t>(self->pendingInlineFootnotePreview.size()));
        }
      }
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving a text-skip scope (zero-height spacer paragraph, or a transparent-text element)
  if (self->skipTextUntilDepth == self->depth) {
    self->skipTextUntilDepth = INT_MAX;
  }

  if (self->currentTable && self->currentTable->depth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    // Degraded: the cell is complete, so lay it out and drop it now. isHeaderRow is only
    // consulted by the grid layout, which this row can no longer reach.
    if (self->currentTable->degraded || self->currentTable->rowDegraded) {
      self->streamClosedCell(self->currentTable->pendingRow);
      self->currentTableCell = nullptr;
      self->nextWordContinues = false;
      return;
    }
    // Determine if the whole row consists of header cells
    {
      auto& row = self->currentTable->pendingRow;
      bool allHeaders = !row.cells.empty();
      for (const auto& c : row.cells) {
        if (!c.isHeader) {
          allHeaders = false;
          break;
        }
      }
      row.isHeaderRow = allHeaders;
    }
    self->currentTableCell = nullptr;
    self->nextWordContinues = false;
  }

  // A caption has to reach the page BEFORE the grid does. The table fragment is only emitted at
  // </table>, so a caption still pending in currentTextBlock is flushed after it and renders
  // BELOW its own table -- which is what happened until a caption long enough to trip the
  // 96-word block split (and only that) accidentally came out in the right order.
  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "caption") == 0) {
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->clearSpentBlockHeadingStyle();  // see the <table> open handler
    self->nextWordContinues = false;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "tr") == 0) {
    // The row is complete: lay it out, pack it, and free its cells. This is the whole point of the
    // streaming model -- residency is one row, not one table.
    self->commitPendingRow();
    self->nextWordContinues = false;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    self->currentTableCell = nullptr;
    // A table whose last row had no </tr> (or no <tr> at all) still has cells pending.
    self->commitPendingRow();
    self->flushTableFragment(self->currentTable->packer);
    self->currentTable.reset();
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Leaving strikethrough tag
  if (self->strikethroughUntilDepth == self->depth) {
    self->strikethroughUntilDepth = INT_MAX;
  }

  // Leaving pre tag
  if (self->preUntilDepth == self->depth) {
    self->preUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  if (!self->pendingInlineFootnotePreview.empty()) {
    std::string preview = " (";
    preview += self->pendingInlineFootnotePreview;
    preview += ")";
    self->pendingInlineFootnotePreview.clear();

    const bool surroundingItalic = self->effectiveItalic;
    self->effectiveItalic = true;
    consumeText(self, preview.c_str(), static_cast<int>(preview.size()), self->visibleTextOffset_, /*source=*/false);
    if (self->partWordBufferIndex > 0 && !self->flushPartWordBuffer()) {
      self->effectiveItalic = surroundingItalic;
      return;
    }
    self->effectiveItalic = surroundingItalic;
  }

  // A block's emphasis went with its style-stack entry, popped above.
  if (headerOrBlockTag) {
    // Reset alignment on empty text blocks to prevent stale alignment from bleeding
    // into the next sibling element. This fixes issue #1026 where an empty <h1> (default
    // Center) followed by an image-only <p> causes Center to persist through the chain
    // of empty block reuse into subsequent text paragraphs.
    // Margins/padding are preserved so parent element spacing still accumulates correctly.
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      // Keep alignment only when closing the <br> separator itself so subsequent text
      // within the same block container stays aligned. Reset alignment when closing
      // other block tags (e.g. div/p) to avoid leaking centered/right alignment globally.
      const bool preserveForBrClose = style.fromBrElement && strcmp(name, "br") == 0;
      if (!preserveForBrClose) {
        style.textAlignDefined = false;
        style.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                              ? CssTextAlign::Justify
                              : static_cast<CssTextAlign>(self->paragraphAlignment);
        self->currentTextBlock->setBlockStyle(style);
      }
    }
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() {
  // An aborted build can leave a page open. Rewind its block now, while the arena is still the
  // section's: BuildState tears the parser down before it releases the feed-chunk block below.
  releasePageBlock();
}

size_t ChapterHtmlSlimParser::anchorLimit() const {
  return anchorSpillWriter.has_value() ? MAX_ANCHORS_PER_CHAPTER : MAX_RESIDENT_ANCHORS;
}

void ChapterHtmlSlimParser::flushAnchorsAwaitingLine() {
  if (anchorsAwaitingLine_.empty()) return;
  for (auto& id : anchorsAwaitingLine_) {
    recordAnchor(std::move(id), static_cast<uint16_t>(completedPageCount));
  }
  anchorsAwaitingLine_.clear();
}

void ChapterHtmlSlimParser::queueAnchorForNextLine(std::string id) {
  if (id.empty()) return;
  // Bounded: an id'd element that never produces a line leaves its entry queued, so a run of them
  // would grow this. Past the cap they are recorded at the current page instead -- the behaviour
  // this replaced, applied only where holding on would cost memory.
  if (anchorsAwaitingLine_.size() >= MAX_ANCHORS_AWAITING_LINE) {
    recordAnchor(std::move(id), static_cast<uint16_t>(completedPageCount));
    return;
  }
  anchorsAwaitingLine_.push_back(std::move(id));
}

// A <span id> used to be left out of the anchor map altogether, to keep converter-injected ids
// out, and every book whose notes are targeted on spans paid for it: the jump missed the map and
// landed on the fallback page, usually the notes chapter's first (crosspoint-reader #3667, #3798).
// Based on crosspoint-reader PR #3799 (Ruslan Sukharev, @skhrv), reworked: theirs records only the
// fragment a jump asks for, so a cached chapter is rebuilt for every note it has not yet been
// asked for; this records every span id once, at build time, within MAX_SPAN_ANCHORS_PER_CHAPTER.
void ChapterHtmlSlimParser::recordSpanAnchor(const std::string& id) {
  // Kobo's KePub conversion gives every sentence a span with a kobo.<paragraph>.<sentence> id, for
  // its own position tracking; no link targets one.
  if (id.rfind("kobo.", 0) == 0) return;
  // Only into the spill. The resident fallback holds the other anchors in RAM under a far lower
  // cap, and spans are not worth a byte of it.
  if (!anchorSpillWriter.has_value()) return;
  if (spanAnchorCount_ >= MAX_SPAN_ANCHORS_PER_CHAPTER) {
    noteCapOverflow(kCapAnchorsPerChapter, "span anchors per chapter");
    return;
  }
  spanAnchorCount_++;
  // A span is inline, so it is usually part of the block in progress, and the next line placed is
  // that block's first: queue it. The exception is a span opened after its block's element has
  // closed but before the next block started (FB2 conversions wrap a whole note,
  // <span id><div><p>..</p></div></span>, in one): the block still in progress is the previous
  // note's, so the id waits for the next block the way any element's does.
  const bool opensNextBlock = currentTextBlock && !currentTextBlock->isEmpty() && currentBlockOwnerDepth_ < 0;
  if (opensNextBlock && pendingAnchorId.empty()) {
    pendingAnchorId = id;
    return;
  }
  queueAnchorForNextLine(id);
}

bool ChapterHtmlSlimParser::lookupAnchorInActiveBuild(const std::string& id, uint16_t& page) {
  if (id.empty() || anchorCount == 0) return false;
  if (!anchorSpillWriter.has_value()) {
    for (const auto& [key, val] : anchorData) {
      if (key == id) {
        page = val;
        return true;
      }
    }
    return false;
  }
  // Commit the buffered tail so a second handle sees every record written so far. SdFat's
  // flush() is a sync (sector + directory entry), the same trick Section::loadPageFromActiveBuild
  // uses on the section file.
  if (!anchorSpillWriter->flush()) return false;
  anchorSpillFile.flush();
  FsFile reader;
  if (!Storage.openFileForRead("EHP", anchorSpillPath, reader)) return false;
  bool found = false;
  std::string key;
  for (uint16_t i = 0; i < anchorCount; ++i) {
    uint16_t recordedPage = 0;
    if (!serialization::readString(reader, key)) break;  // short file: the tail is not there yet
    serialization::readPod(reader, recordedPage);
    if (key == id) {
      page = recordedPage;
      found = true;
      break;
    }
  }
  reader.close();
  return found;
}

void ChapterHtmlSlimParser::recordAnchor(std::string id, const uint16_t page) {
  // The on-disk anchor map counts with a uint16_t, so that is the hard ceiling whatever
  // MAX_ANCHORS_PER_CHAPTER says. Silently dropping past it is the same degradation as the cap:
  // the anchor is not navigable, the chapter still reads.
  if (anchorCount == UINT16_MAX) {
    return;
  }
  if (anchorSpillWriter.has_value()) {
    // Exactly the section cache's anchor-map encoding (u32 length, bytes, u16 page), so
    // Section's finalizer copies these bytes in without re-encoding them.
    if (!anchorSpillWriter->writeString(id) || !anchorSpillWriter->writePod(page)) {
      // A failed write leaves the spill truncated mid-record, which would corrupt every anchor
      // after it. Give up on the map entirely rather than write a map that lies: drop the
      // writer, and let the count go to zero so the finalizer emits an empty map.
      LOG_ERR("EHP", "Anchor spill write failed after %u anchors; dropping the anchor map", anchorCount);
      anchorSpillWriter.reset();
      anchorSpillFile.close();
      anchorSpillFailed = true;
      anchorCount = 0;
      return;
    }
    anchorCount++;
    return;
  }
  if (anchorSpillFailed || anchorCount >= MAX_RESIDENT_ANCHORS) {
    return;
  }
  // Up to MAX_RESIDENT_ANCHORS the vector doubles to a 7 KB block (28 B an entry on the device),
  // through the throwing operator new: without the block, drop the anchor the way the cap does.
  if (!heapReserveFor(anchorData, 1)) {
    noteCapOverflow(kCapAnchorsPerChapter, "resident anchors: no heap block to grow the table");
    return;
  }
  // Counts UP rather than being assigned anchorData.size(), so that a resident anchor recorded
  // after some were already spilled adds to them instead of replacing the total. Section writes
  // the spill first and these after, which is the order they were recorded in.
  anchorData.emplace_back(std::move(id), page);
  anchorCount++;
}

bool ChapterHtmlSlimParser::setup(const size_t totalInflatedSize) {
  initializeFontSizeBaseline();

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD.
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE.
  // Chapter XHTML is HTML-flavored: enable bare-void-tag repair (<br>, <img>, ...).
  if (buildArena_) {
    const size_t bytes = SaxParser::stateBytes();
    if (void* state = buildArena_->alloc(bytes)) {
      saxParser_.setExternalState(state, bytes);
      LOG_DBG("EHP", "SAX parser state (%u bytes) in the build arena", static_cast<unsigned>(bytes));
    }
  }
  if (!saxParser_.init(this, startElement, endElement, characterData, defaultHandlerExpand,
                       /*htmlVoidTagRepair=*/true)) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  totalStreamSize = totalInflatedSize;
  bytesStreamed = 0;
  lastReportedProgress = -1;
  streamFailed = false;
  layoutFailed = false;
  streamStartTimeMs = millis();

  // Pre-size the two tables that grow one entry at a time across the whole parse (this one and
  // Section's page LUT), so a chapter within the estimate takes one block, at setup, instead of
  // a run of chunks interleaved with all the other parse traffic (CLAUDE.md Resource Protocol
  // rule 7, docs/memory-allocation-strategy.md §9.6). It is an allocation-COUNT fix; it is not
  // expected to move contig on its own. Best effort: without the block the first page allocates
  // a small chunk instead.
  paragraphLutPerPage.reserve(estimatePagesForSpine(totalInflatedSize));
  // Anchors stream to a spill file (see setAnchorSpillPath), so nothing here scales with how
  // many a chapter has. Opened at setup rather than lazily so a chapter that cannot spill finds
  // out before it has recorded anything, and takes the resident fallback for all of them or for
  // none -- which keeps the finalizer's two cases from ever both being non-empty.
  if (!anchorSpillPath.empty()) {
    if (Storage.openFileForWrite("EHP", anchorSpillPath, anchorSpillFile)) {
      // 512 B rather than the writer's 4 KB default. An anchor record is ~10-15 bytes, so this
      // still batches ~40 of them per SD write, and the point of the exercise is to stop holding
      // kilobytes for the whole parse -- spending 4 KB to save 28 would be a poor trade.
      anchorSpillWriter.emplace(anchorSpillFile, ANCHOR_SPILL_BUFFER_BYTES);
    } else {
      LOG_ERR("EHP", "Could not open anchor spill %s; holding anchors in RAM", anchorSpillPath.c_str());
    }
  }
  if (!anchorSpillWriter.has_value()) {
    // Fallback only. Reserve for the common case: reserving for the cap would cost ~28 KB up
    // front to save reallocations that only an anchor-heavy chapter would ever pay.
    constexpr size_t TYPICAL_ANCHORS_PER_CHAPTER = 32;
    heapReserveFor(anchorData, TYPICAL_ANCHORS_PER_CHAPTER);  // a hint: recordAnchor asks again
  }

  // Choose progress granularity by chapter size. Each callback drives a full-screen
  // e-ink refresh (~640ms), so smaller chapters skip mid-parse ticks entirely.
  // progressStepPercent == 0 means "popup only, no mid-parse updates".
  progressStepPercent = 0;
  if (totalStreamSize >= SIZE_FOR_PROGRESS_FINE) {
    progressStepPercent = 25;
  } else if (totalStreamSize >= SIZE_FOR_PROGRESS_HEARTBEAT) {
    progressStepPercent = 50;
  }

  const uint32_t popupFreeHeap = ESP.getFreeHeap();
  const uint32_t popupContigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  progressUiEnabled =
      popupFreeHeap >= MIN_FREE_HEAP_FOR_INDEXING_POPUP && popupContigHeap >= MIN_CONTIG_HEAP_FOR_INDEXING_POPUP;
  if (!progressUiEnabled) {
    LOG_DBG("EHP", "Skipping indexing popup due to low heap (free=%u contig=%u)", popupFreeHeap, popupContigHeap);
    // When popup is disabled, also disable mid-parse ticks.
    progressStepPercent = 0;
  }

  // Show initial progress popup for files above threshold.
  if (progressFn && progressUiEnabled && totalStreamSize >= MIN_SIZE_FOR_POPUP) {
    progressFn(0);
  }
  return true;
}

size_t ChapterHtmlSlimParser::write(const uint8_t data) { return write(&data, 1); }

size_t ChapterHtmlSlimParser::write(const uint8_t* buffer, const size_t size) {
  if (size == 0) return 0;
  if (!saxParser_.isActive() || streamFailed) return 0;

  bytesStreamed += size;
  // The streaming source doesn't know "this was the last chunk" — pass isFinal=false
  // here and let finalize() emit the terminating empty parse with isFinal=true.
  if (!saxParser_.feed(buffer, size)) {
    LOG_ERR("EHP", "Parse error at line %d:\n%s", saxParser_.errorLine(), saxParser_.errorString());
    streamFailed = true;
    return 0;
  }

  // Report progress at the granularity chosen up-front (see progressStepPercent).
  // Skip the 100% callback — the page render that follows immediately replaces the popup,
  // so the final tick is wasted work.
  if (progressFn && progressUiEnabled && progressStepPercent > 0 && totalStreamSize > 0) {
    const int progress = static_cast<int>(bytesStreamed * 100 / totalStreamSize);
    if (progress < 100 && progress / progressStepPercent > lastReportedProgress / progressStepPercent) {
      lastReportedProgress = progress;
      progressFn(progress);
    }
  }

  return size;
}

bool ChapterHtmlSlimParser::finalize() {
  bool success = !streamFailed;
  if (saxParser_.isActive()) {
    // Emit terminating empty parse so the parser finalizes any pending tokens.
    if (success && !saxParser_.finalize()) {
      LOG_ERR("EHP", "Parse error at line %d (finalize):\n%s", saxParser_.errorLine(), saxParser_.errorString());
      success = false;
      streamFailed = true;
    }
  }

  const uint32_t totalTimeMs = millis() - streamStartTimeMs;
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", totalTimeMs);

  // The yxml SaxParser backend uses fixed-capacity buffers sized from measured
  // real-world maxima; if this chapter exceeded any of them the parser silently
  // truncated (dropped) the excess. Surface it so out-of-bounds documents are
  // diagnosable rather than failing invisibly (e.g. XPath/anchor drift). Also
  // surfaces voidTag: the source had an HTML-style unclosed void element
  // (<br>, <hr>, ...) that the parser auto-closed rather than failing on, and
  // trailingData: the file had padding bytes after </html> that were ignored.
  if (const uint32_t trunc = saxParser_.truncationFlags()) {
    LOG_DBG("EHP",
            "SaxParser hit fixed-capacity limits (flags=0x%lx): elemName=%d attrName=%d attrVal=%d maxAttrs=%d "
            "maxDepth=%d voidTag=%d trailingData=%d",
            static_cast<unsigned long>(trunc), (trunc & SaxParser::kTruncElemName) != 0,
            (trunc & SaxParser::kTruncAttrName) != 0, (trunc & SaxParser::kTruncAttrValue) != 0,
            (trunc & SaxParser::kTruncMaxAttrs) != 0, (trunc & SaxParser::kTruncMaxDepth) != 0,
            (trunc & SaxParser::kVoidTagRepaired) != 0, (trunc & SaxParser::kTrailingDataIgnored) != 0);
    // Of these, only the depth cap changes what the reader shows (the tree is flattened past
    // 64 levels); the others truncate names and attribute values the layout does not use.
    if (trunc & SaxParser::kTruncMaxDepth) noteCapOverflow(kCapSaxDepth, "element nesting depth");
  }

  // Process last page if there is still text. Done unconditionally so that a partial
  // success scenario still flushes whatever pages were produced.
  if (currentTextBlock) {
    makePages();
    if (!layoutFailed) {
      const bool hasFinalPageContent = currentPage && !currentPage->elements.empty();
      if (!pendingAnchorId.empty()) {
        uint16_t anchorPage = static_cast<uint16_t>(completedPageCount);
        // Avoid mapping trailing anchors to a non-existent blank page when the
        // chapter ended exactly on a page boundary.
        if (!hasFinalPageContent && completedPageCount > 0) {
          anchorPage = static_cast<uint16_t>(completedPageCount - 1);
        }
        recordAnchor(std::move(pendingAnchorId), anchorPage);
        pendingAnchorId.clear();
      }
      if (hasFinalPageContent) {
        emitPage();
      }
    }
    // Both borrow elements of currentPage, and the layoutFailed / empty-final-page paths
    // reach here without going through emitPage(), which is the other place they are cleared.
    deferredPageImage_ = nullptr;
    deferredDropCapLine_ = nullptr;
    currentPage.reset();
    releasePageBlock();
    currentTextBlock.reset();
  }

  // Anything still waiting on a line never got one -- an id'd element with no text of its own.
  // Record those at the last page reached, which is where they sit, before the spill closes.
  flushAnchorsAwaitingLine();

  // Close the anchor spill LAST. The trailing-anchor recording just above is the final call into
  // recordAnchor, and closing before it would send that one anchor down the resident fallback --
  // which is how the whole map was lost once: the fallback took over for that single anchor and
  // Section wrote its count instead of the 544 already on disk.
  if (anchorSpillWriter.has_value()) {
    // The writer's buffered tail has to reach disk before Section copies the file; a flush that
    // fails leaves it short of what anchorCount promises, which would make the copied map run
    // off its own end into the printed-page map that follows it.
    const bool flushed = anchorSpillWriter->flush();
    anchorSpillWriter.reset();
    anchorSpillFile.close();
    if (!flushed) {
      LOG_ERR("EHP", "Anchor spill flush failed; dropping the anchor map");
      anchorSpillFailed = true;
      anchorCount = 0;
    }
  }

  return success;
}

void ChapterHtmlSlimParser::resolveBlockFont(BlockStyle& bs) {
  if (bs.fontResolved) return;
  bs.fontResolved = true;
  if (bs.headingFontId != 0 || bs.fontSizeMultiplier == 1.0f) return;
  const FontSizeLadder::Resolved r = fontSizeLadder_.resolve(bs.fontSizeMultiplier * 100.0f);
  if (r.fontId == 0) {
    // Nearest rung is the body font (or the ladder is empty, e.g. SD fonts):
    // keep the pure-scale path — identical to the legacy behavior.
    bs.fontSizeMultiplier = r.residual;
    return;
  }
  if (!fontSizeLadder_.native()) {
    if (auxFontId_ == 0) auxFontId_ = r.fontId;
    if (r.fontId != auxFontId_) return;  // aux budget already claimed by another size — scale fallback
  }
  bs.headingFontId = r.fontId;
  bs.fontSizeMultiplier = r.residual;
}

int ChapterHtmlSlimParser::effectiveLineHeight(const BlockStyle& bs) const {
  return static_cast<int>(renderer.getLineHeight(effectiveFontId(bs)) * lineCompression * bs.fontSizeMultiplier + 0.5f);
}

ParsedText::LineProcessResult ChapterHtmlSlimParser::addLineToPage(std::unique_ptr<TextBlock> line,
                                                                   const bool lineEndsWithHyphenatedWord,
                                                                   const bool suppressHyphenationRetry,
                                                                   const uint32_t lineVisibleOffset) {
  // Spacing (insets, float zones) lives on the ParsedText that produced this line,
  // not on the line itself: a TextBlock only keeps the render slice, because by the
  // time a line is resident the spacing has already been baked into its xpos/y here.
  // Every layoutAndExtractLines() call that routes into addLineToPage lays out
  // currentTextBlock (the table-cell path moves the cell into it via
  // startNewTextBlock first), so this is the same style the line was built from.
  static const BlockStyle kNoBlockStyle;
  const BlockStyle& lineStyle = currentTextBlock ? currentTextBlock->getBlockStyle() : kNoBlockStyle;

  // Lines carrying inline-sized words advance by the tallest word on the line
  // (microreader semantics); uniform lines keep the block line height exactly.
  int lineHeight = effectiveLineHeight(lineStyle);
  const uint8_t maxPct = line->maxSizePct();
  if (maxPct != 100) {
    lineHeight = lineHeight * maxPct / 100;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight || keepTogetherBreak(lineHeight)) {
    emitPage();
  }

  // The page's last line must not end in a hyphenated word: also when the page ends early, before
  // the next line, to keep a widow or an orphan off it.
  const bool noRoomForAnotherLine =
      currentPageNextY + lineHeight <= viewportHeight &&
      (currentPageNextY + (lineHeight * 2) > viewportHeight || keepTogetherBreak(lineHeight, /*nextLine=*/true));
  if (lineEndsWithHyphenatedWord && !suppressHyphenationRetry && noRoomForAnotherLine) {
    const std::string linePreview = buildTextBlockPreview(line.get());
    LOG_TRC("EHP", "Requesting line rerender without hyphenation to avoid page-break split word: %s",
            linePreview.c_str());
    return ParsedText::LineProcessResult::RetryWithoutHyphenation;
  }

  // Every emitPage() that this line could trigger is above, so completedPageCount is now the page
  // the line is going on: the page any anchor waiting on it should name.
  flushAnchorsAwaitingLine();

  // Capture first-line flag before incrementing wordsExtractedInBlock.
  const bool isFirstLineOfBlock = (wordsExtractedInBlock == 0);

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    if (!currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href)) {
      noteCapOverflow(kCapFootnotesPerPage, "footnote links per page");
    }
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset.
  // For lines that overlap an active left float zone, also shift right by the zone
  // width so text starts after the image rather than overlapping it.
  // Right-floated zones narrow the line width (handled in widthForLine) but don't shift text left.
  int16_t xOffset = lineStyle.leftInset();
  {
    const auto& bs = lineStyle;
    for (int zi = 0; zi < bs.floatZoneCount; ++zi) {
      const auto& z = bs.floatZones[zi];
      if (!z.isRight && currentPageNextY < z.bottom && currentPageNextY + lineHeight > z.top) {
        xOffset = static_cast<int16_t>(xOffset + z.width);
      }
    }
  }
  auto pageLine = makeUniqueNoThrow<PageLine>(std::move(line), xOffset, currentPageNextY);
  if (!pageLine) {
    // Accepted, not a retry: the line is gone either way, and retrying would just fail again.
    LOG_ERR("EHP", "Dropping line: PageLine allocation failed");
    return ParsedText::LineProcessResult::Accepted;
  }
  noteElementOnPage(lineVisibleOffset);
  currentPage->elements.push_back(std::move(pageLine));
  const bool paragraphStart = currentTextBlock && currentTextBlock->linePlace().paragraphStart;
  paragraphLinesOnPage_ = static_cast<uint16_t>((paragraphStart ? 0 : paragraphLinesOnPage_) + 1);

  // On the first line of a block with a deferred inline image, fix the image's
  // yPos so its top aligns with the glyph top of the first text line.
  // PageLine y and image y both use the same coordinate: the line's top edge.
  // Float zones were already pre-corrected in makePages() to the same value.
  if (isFirstLineOfBlock && deferredPageImage_) {
    deferredPageImage_->yPos = static_cast<int16_t>(currentPageNextY);
    deferredPageImage_ = nullptr;
  }

  // Same deferred fix for a drop cap: re-base to the first line's top, keeping the
  // ink-alignment offset computed in finalizePendingDropCap.
  if (isFirstLineOfBlock && deferredDropCapLine_) {
    deferredDropCapLine_->yPos = static_cast<int16_t>(currentPageNextY + dropCapYAdjust_);
    deferredDropCapLine_ = nullptr;
  }

  currentPageNextY += lineHeight;
  return ParsedText::LineProcessResult::Accepted;
}

// Strip heading sizing from a block that makePages() has already drained, so the next block
// does not inherit it through startNewTextBlock's empty-block merge. See the <table> handler.
void ChapterHtmlSlimParser::applyHeadingScope(BlockStyle& blockStyle, const CssStyle& cssStyle) const {
  // The same rule the heading's own alignment follows: centered unless the reader is set to the
  // book's style and the book aligns this block itself.
  const bool booksOwnAlignment =
      embeddedStyle && cssStyle.hasTextAlign() && paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None);
  if (!booksOwnAlignment) blockStyle.alignment = headingAlignment_;
  blockStyle.textAlignDefined = true;
  blockStyle.keepWithNext = true;
  if (!cssStyle.hasFontSizeMultiplier()) blockStyle.fontSizeMultiplier = headingFontSizeMultiplier_;
}

void ChapterHtmlSlimParser::startBlockSpan(const CssStyle& cssStyle, BlockStyle blockStyle) {
  blockSpan_ = BlockSpan{};
  blockSpan_.depth = depth;
  blockSpan_.interruptedOwnerDepth = currentBlockOwnerDepth_;
  if (currentTextBlock) {
    const BlockStyle& interrupted = currentTextBlock->getBlockStyle();
    blockSpan_.alignment = interrupted.alignment;
    blockSpan_.textAlignDefined = interrupted.textAlignDefined;
    blockSpan_.fontSizeMultiplier = interrupted.fontSizeMultiplier;
    blockSpan_.headingFontId = interrupted.headingFontId;
    blockSpan_.fontResolved = interrupted.fontResolved;
  }

  // From here on the span is laid out as a <div> would be.
  if (embeddedStyle && cssStyle.hasTextAlign() && paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
    blockStyle.alignment = cssStyle.textAlign;
    blockStyle.textAlignDefined = true;
  }
  if (depth > headingDepth_) applyHeadingScope(blockStyle, cssStyle);
  holdBottomSpacing(blockStyle, /*floated=*/false);
  startNewTextBlock(blockStyle);
  currentBlockOwnerDepth_ = depth;
}

void ChapterHtmlSlimParser::endBlockSpan() {
  // Alignment, size and insets of the element the span interrupted, not its vertical spacing:
  // that was spent where the element started. The size fields travel together, as for <br>.
  BlockStyle resumed;
  resumed.alignment = blockSpan_.alignment;
  resumed.textAlignDefined = blockSpan_.textAlignDefined;
  resumed.fontSizeMultiplier = blockSpan_.fontSizeMultiplier;
  resumed.headingFontId = blockSpan_.headingFontId;
  resumed.fontResolved = blockSpan_.fontResolved;
  resumed.keepWithNext = depth > headingDepth_;  // the rest of a heading the span sat in
  addAncestorInsets(resumed, static_cast<float>(renderer.getFontAscenderSize(fontId)));

  if (currentTextBlock && currentTextBlock->isEmpty()) {
    // Nothing of the span reached its block. Its spacing stays there for whatever follows to
    // merge with; only what it would lend that follower is replaced.
    BlockStyle style = currentTextBlock->getBlockStyle();
    style.alignment = resumed.alignment;
    style.textAlignDefined = resumed.textAlignDefined;
    style.fontSizeMultiplier = resumed.fontSizeMultiplier;
    style.headingFontId = resumed.headingFontId;
    style.fontResolved = resumed.fontResolved;
    style.marginLeft = resumed.marginLeft;
    style.marginRight = resumed.marginRight;
    style.paddingLeft = resumed.paddingLeft;
    style.paddingRight = resumed.paddingRight;
    currentTextBlock->setBlockStyle(style);
  } else {
    startNewTextBlock(resumed);
  }
  currentBlockOwnerDepth_ = blockSpan_.interruptedOwnerDepth;
  blockSpan_.depth = INT_MAX;
}

void ChapterHtmlSlimParser::clearSpentBlockHeadingStyle() {
  if (!currentTextBlock || !currentTextBlock->isEmpty()) return;
  BlockStyle& spent = currentTextBlock->getBlockStyle();
  spent.fontSizeMultiplier = 1.0f;
  spent.headingFontId = 0;
  spent.fontResolved = false;
  spent.keepWithNext = false;
}

void ChapterHtmlSlimParser::makePages() {
  if (layoutFailed) {
    currentTextBlock.reset();
    return;
  }

  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  // Snap the block to the size ladder before any metric below is computed. Uniform
  // per-word sizes (a span wrapping the whole paragraph) fold into the block multiplier
  // first so they benefit too; continuations skip the fold — their first chunk already
  // laid out with the resolved style, and resolveBlockFont is a no-op on them anyway.
  if (!currentTextBlock->isContinuation()) {
    currentTextBlock->foldUniformWordSizes();
  }
  resolveBlockFont(currentTextBlock->getBlockStyle());

  if (!applyBlockTopSpacing()) {
    currentTextBlock.reset();
    return;
  }

  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int lineHeight = effectiveLineHeight(blockStyle);

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  if (!ensureHeapForTextLayout("paragraph layout", currentTextBlock.get())) {
    layoutFailed = true;
    currentTextBlock.reset();
    return;
  }

  // Active-float propagation. A tall floated image spans several text blocks (its
  // caption plus the following paragraphs). The image is attached to the first of
  // those blocks; here we re-inject the same zone into every later block that still
  // overlaps the image vertically, so they all wrap beside it — then drop it once
  // layout has passed the image bottom.
  const bool isOriginatingBlock = static_cast<bool>(deferredPageImage_) || static_cast<bool>(deferredDropCapLine_);
  if (activeFloatBottom_ > 0 && currentPageNextY >= activeFloatBottom_) {
    activeFloatBottom_ = 0;  // layout has moved past the image; float no longer applies
  }
  if (!isOriginatingBlock && activeFloatBottom_ > 0 && currentPageNextY < activeFloatBottom_ &&
      currentTextBlock->getBlockStyle().floatZoneCount == 0) {
    BlockStyle& mbs = currentTextBlock->getBlockStyle();
    auto& z = mbs.floatZones[mbs.floatZoneCount++];
    z.top = activeFloatTop_;  // absolute (already-anchored) image coordinates
    z.bottom = activeFloatBottom_;
    z.width = activeFloatWidth_;
    z.isRight = activeFloatIsRight_;
  }

  // Pre-correct float zone coordinates before line-breaking so widthForLine
  // and the xOffset check in addLineToPage use the same y values. Only the
  // originating block re-anchors (its zone, and the image, snap to the first
  // line top); injected zones already carry absolute image coordinates.
  const int lineHeightForFloat = (blockStyle.floatZoneCount > 0) ? effectiveLineHeight(blockStyle) : 0;
  if (isOriginatingBlock && blockStyle.floatZoneCount > 0) {
    auto& mbs = currentTextBlock->getBlockStyle();
    for (int zi = 0; zi < mbs.floatZoneCount; ++zi) {
      const int imgH = mbs.floatZones[zi].bottom - mbs.floatZones[zi].top;
      mbs.floatZones[zi].top = static_cast<int16_t>(currentPageNextY);
      mbs.floatZones[zi].bottom = static_cast<int16_t>(currentPageNextY + imgH);
    }
    // Finalise the active-float extent so following blocks reference the image's
    // real on-page position (after this block's top margin).
    activeFloatTop_ = static_cast<int16_t>(currentPageNextY);
    activeFloatBottom_ = static_cast<int16_t>(currentPageNextY + (mbs.floatZones[0].bottom - mbs.floatZones[0].top));
  }
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](std::unique_ptr<TextBlock> textBlock, const bool lineEndsWithHyphenatedWord,
             const bool suppressHyphenationRetry) {
        return addLineToPage(std::move(textBlock), lineEndsWithHyphenatedWord, suppressHyphenationRetry,
                             currentTextBlock->lastLineVisibleOffset());
      },
      /*includeLastLine=*/true, static_cast<int16_t>(currentPageNextY), lineHeightForFloat);

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      if (!currentPage->addFootnote(fn.number, fn.href))
        noteCapOverflow(kCapFootnotesPerPage, "footnote links per page");
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
    lastBlockMarginBottom = blockStyle.marginBottom;
  } else {
    lastBlockMarginBottom = 0;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior).
  // Suppressed between lines within a <pre> block so code/preformatted text is not
  // double-spaced; the last line of the block is flushed after </pre> is closed and
  // preUntilDepth has already been reset, so it still receives normal paragraph spacing.
  if (extraParagraphSpacing && preUntilDepth == INT_MAX) {
    currentPageNextY += lineHeight / 2;
  }
}

bool ChapterHtmlSlimParser::applyBlockTopSpacing() {
  // The spacing is an offset into the current page, so the page has to exist first: left to
  // addLineToPage(), the first line would create it and restart y at 0, dropping the spacing.
  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "OOM: page object");
      layoutFailed = true;
      return false;
    }
    currentPage->elements.reserve(Page::TYPICAL_ELEMENTS);
    currentPageNextY = 0;
  }

  // A paragraph over 96 words is laid out in chunks (flushPartWordBuffer), and every chunk after
  // the first is a continuation: its top is already on the page. The first chunk is laid out by
  // the split, so the split has to place the spacing too -- it once did not, and the continuation
  // flag then kept makePages() from catching up, so a long paragraph lost its margin-top,
  // padding-top and any <br> scene-break line stored there.
  //
  // Diagnosed in crosspoint-reader PR #3875 ("fix: preserve paragraph continuity across soft
  // flushes", Phạm Bình An / @brianhuster). Their fix routes the split through makePages(); ours
  // keeps the split's own path (isContinuation() already covers the indent half) and shares this.
  //
  // Not clamped to the page: spacing that runs past the bottom is dropped by the first line's
  // page break in addLineToPage(), which starts the new page at y = 0.
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (!currentTextBlock->isContinuation()) {
    if (blockStyle.marginTop > 0) {
      // CSS margin collapsing: gap between adjacent blocks = max(prevMarginBottom, thisMarginTop).
      // lastBlockMarginBottom was already added after the previous block; subtract the overlap.
      const int16_t collapse = std::min(lastBlockMarginBottom, blockStyle.marginTop);
      currentPageNextY += static_cast<int16_t>(blockStyle.marginTop - collapse);
    }
    if (blockStyle.paddingTop > 0) {
      currentPageNextY += blockStyle.paddingTop;
    }
  }
  lastBlockMarginBottom = 0;
  return true;
}

void ChapterHtmlSlimParser::commitPendingRow() {
  if (!currentTable) return;
  auto& t = *currentTable;

  // Degraded rows already emitted every cell from </td>; there is nothing pending.
  if (t.degraded || t.rowDegraded) {
    t.pendingRow.cells.clear();
    t.pendingRowBytes = 0;
    return;
  }
  if (t.pendingRow.cells.empty()) return;

  if (t.rowOverflowed) {
    degradeRow("column overflow");
    return;
  }

  // Widening rows open a new fragment at the wider count; narrower rows are padded by
  // layoutTableRow and stay aligned with the fragment they are joining. Only the maximum column
  // count ever needed the whole table, and this is how streaming pays for not having it: the
  // count is discovered as rows arrive rather than known up front, so a table that widens
  // halfway widens on screen instead of being laid out wide from its first row.
  const uint8_t columnCount = std::max(t.columnCount, t.pendingRow.effectiveCols);
  if (columnCount == 0 || columnCount > MAX_TABLE_COLS) {
    degradeRow("column count out of range");
    return;
  }

  const uint16_t colWidth = t.contentWidth / columnCount;
  const uint16_t innerColWidth =
      (colWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(colWidth - 2 * TABLE_CELL_PADDING) : 0;
  if (innerColWidth < MIN_COL_INNER_WIDTH) {
    degradeRow("columns too narrow");
    return;
  }

  // Gate before the layout allocations rather than after: a heap dip now costs this row, not the
  // rows already packed into the fragment.
  if (!heapAllowsTableRowLayout()) {
    tableRowDegradedAny_ = true;  // the one demotion the heap causes: latched, escalated, rebuilt
    degradeRow("low heap at row layout");
    return;
  }

  // Arena builds lay the row's cell lines out in the lent region (see layoutTableRow), above the
  // page block, inside a transient block of their own. The block is committed once the row is
  // known to stay on this page; if the row has to open the NEXT page, the block is released,
  // the page emitted (which rewinds its block), and the row laid out again from its preserved
  // source text. That keeps every fragment's bytes inside the block of the page it lands on,
  // which is what lets the page's rewind reclaim them (memory audit 2026-09, run 12: the whole
  // Roosevelt appendix demoted for want of 18 KB of heap while 38 KB of the arena sat idle).
  ensurePageBlock();
  LayoutRow lr;
  BuildArena::Block rowBlock;
  if (buildArena_) rowBlock = buildArena_->reserveBlock();
  const auto dropRowLayout = [&] {
    lr.cells.clear();
    if (rowBlock.valid() && !buildArena_->release(rowBlock)) {
      LOG_ERR("EHP", "Table row arena block could not be released (out of order); region stays claimed");
    }
  };
  if (!layoutTableRow(t.pendingRow, columnCount, lr)) {
    dropRowLayout();
    degradeRow("row cannot be a grid row");
    return;
  }
  t.columnCount = columnCount;

  // The row's first word: where a fragment this row opens starts, and what a wordless fragment
  // closed below stands before. Taken now, before the header capture further down moves the
  // cells out.
  uint32_t rowStart = UINT32_MAX;
  for (const auto& cell : t.pendingRow.cells) {
    if (cell.text) {
      if (const auto first = cell.text->firstWordVisibleOffset()) rowStart = std::min(rowStart, *first);
    }
  }
  const auto flushFragmentBeforeRow = [&] {
    const PinnedPlacement beforeRow(*this, rowStart != UINT32_MAX ? rowStart : pinnedPlacementOffset_);
    flushTableFragment(t.packer);
  };

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // A change in column count requires a new fragment; each PageTableFragment carries exactly one.
  if (!t.packer.rows.empty() && lr.renderCols != t.packer.cols) {
    flushFragmentBeforeRow();
  }
  if (t.packer.cols == 0) t.packer.cols = lr.renderCols;

  const uint16_t rowContrib = t.packer.hasBorder ? static_cast<uint16_t>(lr.height + 1) : lr.height;
  // The fragment's closing border line, counted in the fit test so a packed fragment always fits
  // the page it was packed on: flushTableFragment then never has to move it to the next page --
  // which an arena build could not do, its bytes being in this page's block.
  const uint16_t closingBorder = t.packer.hasBorder ? 1 : 0;

  // Height the repeated header adds when this row is the one that opens a continuation fragment.
  // Zero for the header row itself, which would otherwise be emitted twice at the top of the table.
  const bool repeatHeaderHere = t.repeatHeader && !lr.isHeaderRow && t.repeatHeaderHeight > 0;
  const uint16_t headerContrib =
      repeatHeaderHere ? (t.packer.hasBorder ? static_cast<uint16_t>(t.repeatHeaderHeight + 1) : t.repeatHeaderHeight)
                       : 0;

  // MAX_TABLE_ROWS used to bound the whole table, which is what kept PageTableFragment::deserialize
  // from ever seeing an over-long fragment (Page.cpp rejects rowCount > MAX_TABLE_ROWS). Tables are
  // no longer bounded, so the cap has to live here, on the fragment that actually has to satisfy
  // it. The viewport check below reaches it first at any realistic row height; this is the
  // invariant, not the working limit.
  if (!t.packer.rows.empty() && (t.packer.rows.size() >= MAX_TABLE_ROWS ||
                                 currentPageNextY + t.packer.height + rowContrib + closingBorder > viewportHeight)) {
    flushFragmentBeforeRow();
    t.packer.cols = lr.renderCols;
  }

  // If what is left of the page cannot hold even this one row, break now. Without this the row
  // opens a fragment in the few pixels left at the bottom, the next row immediately has to flush
  // it, and the table arrives on the next page split into a one-row box followed by the rest --
  // two bordered boxes where the reader should see one continuous table. Only reachable now that
  // tables span pages as grids rather than flattening at 48 rows.
  if (t.packer.rows.empty() && currentPageNextY > 0 &&
      currentPageNextY + headerContrib + rowContrib + closingBorder > viewportHeight) {
    dropRowLayout();  // its bytes were in the page about to be emitted
    emitPage();
    ensurePageBlock();
    if (buildArena_) rowBlock = buildArena_->reserveBlock();
    if (!layoutTableRow(t.pendingRow, columnCount, lr)) {
      dropRowLayout();
      degradeRow("row cannot be a grid row");
      return;
    }
  }
  // The row stays on this page: its bytes now belong to the page block's scope.
  if (rowBlock.valid()) buildArena_->commit(rowBlock);

  // Capture the table's leading header row for the continuation fragments below. Only a row that
  // OPENS the table qualifies (in practice a <thead> row), and only while it is short enough that
  // repeating it does not eat the page it exists to make readable. After the placement above,
  // because a re-layout needs the buffered row this takes.
  if (!t.repeatHeaderResolved) {
    t.repeatHeaderResolved = true;
    if (lr.isHeaderRow && lr.height <= viewportHeight / 3) {
      // The BUFFERED row is kept, not the laid-out one: layoutTableRow preserves its source
      // (preserveSource=true), so the cells can be laid out again for each continuation fragment
      // and every fragment ends up owning its own lines. Height is recorded here so the packing
      // arithmetic below does not have to lay the header out just to measure it.
      //
      // MOVED, not copied -- BufferedTableCell owns a unique_ptr<ParsedText> and is not copyable,
      // and this row's cells are discarded at the end of this function regardless, so taking them
      // costs nothing. The clear() at the bottom then runs on an already-emptied vector.
      auto header = std::unique_ptr<BufferedTableRow>(new (std::nothrow) BufferedTableRow(std::move(t.pendingRow)));
      if (header) {
        t.repeatHeader = std::move(header);
        t.repeatHeaderHeight = lr.height;
      }
    }
  }

  // Reopen a continuation fragment with the table's header row, so the reader still has column
  // labels on every page the table covers rather than only the first.
  if (repeatHeaderHere && t.packer.rows.empty()) {
    // Laid out afresh for this fragment (into this page's block on an arena build). A failure here
    // is not fatal -- the continuation simply opens without a header, which is what every table
    // did before this existed.
    LayoutRow hdrLayout;
    if (layoutTableRow(*t.repeatHeader, columnCount, hdrLayout) && hdrLayout.renderCols == lr.renderCols) {
      TableRow hdr;
      hdr.isHeaderRow = hdrLayout.isHeaderRow;
      hdr.height = hdrLayout.height;
      hdr.cells = std::move(hdrLayout.cells);
      t.packer.rows.push_back(std::move(hdr));
      t.packer.height += headerContrib;
    }
  }

  TableRow tr;
  tr.isHeaderRow = lr.isHeaderRow;
  tr.height = lr.height;
  tr.cells = std::move(lr.cells);
  if (t.packer.firstVisibleOffset == UINT32_MAX) t.packer.firstVisibleOffset = rowStart;
  t.packer.rows.push_back(std::move(tr));
  t.packer.height += rowContrib;

  // A grid row never reaches addLineToPage, so this is the table path's equivalent: the row is
  // now committed and every emitPage() it could have forced is above, making completedPageCount
  // the page it lands on. Without this an anchor on a <table> or a <tr> whose rows all lay out as
  // a grid waited for the next ordinary paragraph instead -- measured ten pages later on a book
  // whose notes are one long table.
  flushAnchorsAwaitingLine();

  // Free the row's words. Everything above exists to make this possible one row at a time.
  t.pendingRow.cells.clear();
  t.pendingRowBytes = 0;
}

std::unique_ptr<ImageBlock> ChapterHtmlSlimParser::buildCellImage(const std::string& src, const std::string& alt,
                                                                  const uint16_t maxWidth, const uint16_t maxHeight) {
  if (src.empty()) return nullptr;
  return buildImageFromEpubPath(FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + src)), alt, maxWidth,
                                maxHeight);
}

std::unique_ptr<ImageBlock> ChapterHtmlSlimParser::buildImageFromEpubPath(
    const std::string& resolvedPath, const std::string& alt, const uint16_t maxWidth, const uint16_t maxHeight,
    uint16_t* nativeWidth, uint16_t* nativeHeight) {
  if (resolvedPath.empty() || maxWidth == 0 || maxHeight == 0) return nullptr;
  if (!isImageCandidate(resolvedPath)) return nullptr;

  ImageDimensions dims = {0, 0};
  bool dimsOk = false;
  if (imageManifest) {
    dimsOk = imageManifest->ensureResolved(epub->getPath(), resolvedPath, dims);
  }
  if (!dimsOk) {
    dimsOk = ImageDecoderFactory::getDimensionsFromZipEntry(epub->getPath(), resolvedPath, dims);
  }
  if (!dimsOk || dims.width == 0 || dims.height == 0) {
    LOG_DBG("EHP", "Image: no dims for %s", resolvedPath.c_str());
    return nullptr;
  }
  if (nativeWidth) *nativeWidth = static_cast<uint16_t>(std::min<uint32_t>(dims.width, UINT16_MAX));
  if (nativeHeight) *nativeHeight = static_cast<uint16_t>(std::min<uint32_t>(dims.height, UINT16_MAX));

  // Scale to fit the cell box, preserving aspect ratio. Never upscale.
  float scale = 1.0f;
  if (static_cast<int>(dims.width) > static_cast<int>(maxWidth)) scale = static_cast<float>(maxWidth) / dims.width;
  if (static_cast<int>(dims.height) * scale > static_cast<int>(maxHeight))
    scale = static_cast<float>(maxHeight) / dims.height;
  const int displayWidth = std::max(1, static_cast<int>(dims.width * scale));
  const int displayHeight = std::max(1, static_cast<int>(dims.height * scale));

  const std::string cachedPath = imageCachePathFor(imageBasePath, resolvedPath);

  return makeUniqueNoThrow<ImageBlock>(cachedPath, static_cast<int16_t>(displayWidth),
                                       static_cast<int16_t>(displayHeight), alt, epub->getPath(), resolvedPath);
}

void ChapterHtmlSlimParser::noteBackgroundImage(const char* name, const std::string& classAttr,
                                                const std::string& idAttr, const std::string& styleAttr) {
  // Only with the book's own styles in play, and only when pictures are shown at all.
  if (!cssParser || imageRendering != 0 || !matches(name, BACKGROUND_PICTURE_TAGS, NUM_BACKGROUND_PICTURE_TAGS)) {
    return;
  }
  // A grid has no room for a block picture between its rows; a table's own picture waits for
  // </table>, which is why the <table> element itself is let through (currentTable is set after).
  if (currentTable || backgroundImageScopes_.size() >= MAX_BACKGROUND_PICTURE_SCOPES) return;

  std::string path;
  bool inlineDeclared = false;
  if (!styleAttr.empty()) {
    const CssParser::BackgroundDeclaration bg = CssParser::parseBackground(styleAttr);
    if (bg.declared) {
      inlineDeclared = true;  // outranks the stylesheet, a `none` included
      if (!bg.url.empty() && bg.noRepeat) path = contentBase + bg.url;
    }
  }
  if (!inlineDeclared) {
    if (const std::string* sheetPath = cssParser->backgroundImageFor(name, classAttr, idAttr)) path = *sheetPath;
  }
  if (path.empty()) return;
  std::string resolved = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(path));
  if (!isImageCandidate(resolved)) return;
  backgroundImageScopes_.push_back({depth, std::move(resolved)});
}

void ChapterHtmlSlimParser::placeBackgroundImage(const std::string& resolvedPath) {
  // The element's text first: the picture follows everything it held.
  if (partWordBufferIndex > 0 && !flushPartWordBuffer()) return;
  if (currentTextBlock && !currentTextBlock->isEmpty()) makePages();
  applyPendingPageBreak();
  uint16_t nativeWidth = 0;
  uint16_t nativeHeight = 0;
  auto image = buildImageFromEpubPath(resolvedPath, {}, static_cast<uint16_t>(viewportWidth),
                                      static_cast<uint16_t>(viewportHeight), &nativeWidth, &nativeHeight);
  if (!image) return;
  if (nativeWidth < MIN_BACKGROUND_PICTURE_SIDE || nativeHeight < MIN_BACKGROUND_PICTURE_SIDE) {
    LOG_DBG("EHP", "Background picture %ux%u is decoration, not placed: %s", nativeWidth, nativeHeight,
            resolvedPath.c_str());
    return;
  }
  LOG_DBG("EHP", "Background picture placed after its block: %s (%ux%u)", resolvedPath.c_str(), nativeWidth,
          nativeHeight);
  placeImageBlockAsBlock(std::move(image));
}

void ChapterHtmlSlimParser::placeImageBlockAsBlock(std::unique_ptr<ImageBlock> image) {
  if (!image) return;
  const int displayWidth = image->getWidth();
  const int displayHeight = image->getRenderedHeight();

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }
  if (!currentPage->elements.empty() && currentPageNextY + displayHeight > viewportHeight) {
    emitPage();
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
  }

  const int xPos = (viewportWidth - displayWidth) / 2;

  if (displayHeight <= viewportHeight) {
    auto pageImage = makeUniqueNoThrow<PageImage>(std::move(image), xPos, currentPageNextY);
    if (!pageImage) {
      LOG_ERR("EHP", "Image dropped: PageImage allocation failed");
      return;
    }
    noteElementOnPage(placementOffset());
    currentPage->elements.push_back(std::move(pageImage));
    currentPageNextY += displayHeight;
    LOG_TRC("EHP", "Image placed as block: %dx%d", displayWidth, displayHeight);
    return;
  }

  // Image taller than one page: split into per-page crops. The final slice must be at
  // least kMinImageSliceH pixels tall to avoid a near-invisible sliver on the last page;
  // if it would be shorter, absorb it into the preceding slice by shortening that slice.
  static constexpr int kMinImageSliceH = 64;
  int srcOffset = 0;
  while (srcOffset < displayHeight) {
    int remaining = displayHeight - srcOffset;
    int sliceH = std::min(remaining, static_cast<int>(viewportHeight));
    // If the leftover after this slice would be a sliver smaller than kMinImageSliceH,
    // shrink the current slice to leave exactly kMinImageSliceH for the next page.
    int leftover = remaining - sliceH;
    if (leftover > 0 && leftover < kMinImageSliceH) {
      sliceH -= (kMinImageSliceH - leftover);
    }
    auto crop = image->makeCrop(static_cast<int16_t>(srcOffset), static_cast<int16_t>(sliceH));
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }
    auto slice = makeUniqueNoThrow<PageImage>(std::move(crop), xPos, currentPageNextY);
    if (!slice) {
      LOG_ERR("EHP", "Image slice dropped: PageImage allocation failed");
      return;
    }
    noteElementOnPage(placementOffset());
    currentPage->elements.push_back(std::move(slice));
    currentPageNextY += sliceH;
    srcOffset += sliceH;
    LOG_DBG("EHP", "Image slice placed: offset=%d h=%d", srcOffset - sliceH, sliceH);
    if (srcOffset < displayHeight) {
      emitPage();
    }
  }
}

void ChapterHtmlSlimParser::degradeRow(const char* reason) {
  if (!currentTable) return;
  auto& t = *currentTable;
  if (t.rowDegraded) return;

  LOG_DBG("EHP", "Table row degraded to paragraphs (%s): %u buffered cell(s), %u buffered byte(s)", reason,
          static_cast<unsigned>(t.pendingRow.cells.size()), static_cast<unsigned>(t.pendingRowBytes));

  // Close the fragment first: the paragraphs about to be emitted come AFTER every row already
  // packed, and a fragment written later would land below them on the page. A wordless fragment
  // stands before this row's first word; the counter (or the caller's pin) is already past it.
  {
    uint32_t rowStart = placementOffset();
    for (const auto& cell : t.pendingRow.cells) {
      if (cell.text) {
        if (const auto first = cell.text->firstWordVisibleOffset()) rowStart = std::min(rowStart, *first);
      }
    }
    const PinnedPlacement beforeRow(*this, rowStart);
    flushTableFragment(t.packer);
  }

  t.rowDegraded = true;
  t.pendingRowBytes = 0;

  // Text first, then images, matching the order the batch fallback always used. Callers only reach
  // this with no cell open (<table>, <td>, </tr>); the mid-cell case goes through
  // degradeRowAtOpenCell(), so clearing currentTableCell here cannot orphan a cell mid-fill.
  // The return is deliberately dropped: degradeRow() is reached from void SAX handlers, and a
  // heap guard that stops the parse has already latched streamFailed, which every handler checks.
  // Only degradeRowAtOpenCell() propagates, because its caller can still refuse the word.
  (void)emitRowAsParagraphs(t.pendingRow);
  emitRowImagesAsBlocks(t.pendingRow);
  t.pendingRow.cells.clear();
  t.pendingRow.cells.shrink_to_fit();
  currentTableCell = nullptr;
}

void ChapterHtmlSlimParser::degradeTable(const char* reason) {
  if (!currentTable) return;
  degradeRow(reason);
  currentTable->degraded = true;
}

bool ChapterHtmlSlimParser::degradeRowAtOpenCell(const char* reason) {
  if (!currentTable || currentTable->rowDegraded || currentTable->degraded || !currentTableCell) return true;
  auto& t = *currentTable;
  if (t.pendingRow.cells.empty()) return true;

  // degradeRow() cannot be used here: it drains every buffered cell and clears the row, which
  // would destroy the cell we are still filling. Lift the open cell out first, degrade the rest
  // exactly as the normal path does, then emit the open cell's accumulated words and hand it back
  // empty so the words still arriving keep flowing into the same cell.
  BufferedTableCell open = std::move(t.pendingRow.cells.back());
  t.pendingRow.cells.pop_back();
  currentTableCell = nullptr;  // nothing may route into the detached cell during the drain

  {
    // The earlier cells' images go out ahead of the open cell's words, so they stand before its
    // first word, not before the word whose flush brought us here.
    std::optional<uint32_t> openStart;
    if (open.text) openStart = open.text->firstWordVisibleOffset();
    const PinnedPlacement beforeOpenCell(*this, openStart ? *openStart : placementOffset());
    degradeRow(reason);
  }

  // The open cell's words come after everything buffered before it, in document order. Its image
  // does NOT go out here — a degraded cell emits its image at </td>, and `open` keeps it.
  bool ok = true;
  if (open.text && !open.text->isEmpty()) {
    BufferedTableCell chunk;
    chunk.text = std::move(open.text);
    chunk.isHeader = open.isHeader;
    ok = emitCellAsParagraph(chunk, /*emitImage=*/false);
  }
  open.text = std::unique_ptr<ParsedText>(new ParsedText(false, false));

  t.pendingRow.cells.push_back(std::move(open));
  t.pendingRow.effectiveCols = 0;
  currentTableCell = &t.pendingRow.cells.back();
  return ok;
}

void ChapterHtmlSlimParser::streamClosedCell(BufferedTableRow& row) {
  if (row.cells.empty()) return;
  emitCellAsParagraph(row.cells.back(), /*emitImage=*/true);
  row.cells.pop_back();  // the cell is fully consumed; never accumulates
}

void ChapterHtmlSlimParser::emitRowImagesAsBlocks(BufferedTableRow& row) {
  // Paragraph path: cells are flattened to paragraphs, so any cell image is emitted as a
  // full-width block image below them, one per line — same as any other block image. Runs after
  // emitRowAsParagraphs, which has already released every cell's ParsedText, so the image decodes
  // below get the heap the text was holding.
  for (auto& cell : row.cells) {
    if (cell.imageSrc.empty()) continue;
    placeImageBlockAsBlock(buildCellImage(cell.imageSrc, cell.imageAlt, viewportWidth, viewportHeight));
  }
}

bool ChapterHtmlSlimParser::layoutTableRow(BufferedTableRow& bufRow, const uint8_t columnCount, LayoutRow& out) {
  // Every row is laid out on the same `columnCount` grid; a colspan cell simply covers `colSpan`
  // of those columns. Row widths use the floor'd column width the renderer also starts from, so a
  // span is never laid out wider than the box it will be drawn in.
  const uint16_t colWidth = currentTable->contentWidth / columnCount;

  // Cap an in-cell graphic so it never alone overflows the viewport (which would force the
  // paragraph fallback and drop the image).
  const uint16_t cellImageMaxHeight = static_cast<uint16_t>(
      std::min<int>(MAX_CELL_IMAGE_HEIGHT, std::max<int>(1, viewportHeight - 2 * TABLE_CELL_PADDING)));
  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);

  out.cells.clear();
  out.isHeaderRow = bufRow.isHeaderRow;
  out.renderCols = columnCount;
  out.cells.reserve(columnCount);  // spans sum to columnCount, so that is the padded upper bound
  uint16_t maxContentHeight = 0;   // tallest cell's text + image, in pixels
  uint8_t col = 0;                 // first grid column of the cell being laid out

  for (auto& bufCell : bufRow.cells) {
    TableCell cell;
    cell.isHeader = bufCell.isHeader;
    // A row whose spans overrun the grid cannot be represented; effectiveCols feeds columnCount,
    // so this only fires on a malformed row that changed shape after the count was taken.
    const uint8_t span = bufCell.colSpan ? bufCell.colSpan : 1;
    if (col + span > columnCount) {
      LOG_DBG("EHP", "Table row spans past its %u columns — falling back to paragraphs",
              static_cast<unsigned>(columnCount));
      return false;
    }
    cell.colSpan = span;
    const uint16_t renderColWidth = static_cast<uint16_t>(span * colWidth);
    const uint16_t renderInnerWidth =
        (renderColWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(renderColWidth - 2 * TABLE_CELL_PADDING) : 0;
    const uint16_t cellImageMaxWidth =
        (renderInnerWidth > 0) ? renderInnerWidth : static_cast<uint16_t>(MIN_COL_INNER_WIDTH);
    col = static_cast<uint8_t>(col + span);

    if (bufCell.text && !bufCell.text->isEmpty()) {
      // Count past the cap rather than stopping at it: the overflow itself is the signal, and
      // the grid cannot represent this cell either way.
      size_t producedLines = 0;
      // Line bytes from the lent region on an arena build. No page-fit hook: the row is placed as
      // a whole by the caller, inside a block it commits or releases (see the row emission).
      bufCell.text->setLineArena(buildArena_);
      bufCell.text->setBeforeLineHook(nullptr);
      bufCell.text->layoutAndExtractLines(
          renderer, fontId, renderInnerWidth,
          [&cell, &producedLines](std::unique_ptr<TextBlock> tb, bool, bool) {
            ++producedLines;
            // Stop collecting past the cap: the cell is going to the paragraph fallback anyway,
            // and a cell that needs 139 lines would otherwise build 139 TextBlocks to throw away.
            if (cell.lines.size() < MAX_CELL_LINES) {
              cell.lines.push_back(std::move(tb));
            }
            return ParsedText::LineProcessResult::Accepted;
          },
          /*includeLastLine=*/true, /*blockStartY=*/0, /*lineHeight=*/0, /*preserveSource=*/true);
      // A cell that lays out to more lines than the grid can carry used to be TRUNCATED here --
      // silently, with no log and no fallback, so the tail of the cell was simply deleted from
      // the book. Measured on alice-illustrated: 189 words lost from one cell. Fall back to
      // paragraphs instead, which is what the colspan check above already does for a table the
      // grid cannot represent.
      //
      // This only works because of the preserveSource argument above. Without it the layout call
      // that DETECTS the overflow also erases the words it laid out, so the caller's fallback
      // would find this cell -- and every cell laid out before it -- already empty, and would
      // emit nothing at all. Nothing has been written to a page from here, so bailing is clean.
      if (producedLines > MAX_CELL_LINES) {
        LOG_DBG("EHP", "Table cell needs %u lines (max %u) — falling back to paragraphs",
                static_cast<unsigned>(producedLines), static_cast<unsigned>(MAX_CELL_LINES));
        return false;
      }
    }

    if (!bufCell.imageSrc.empty()) {
      cell.image = buildCellImage(bufCell.imageSrc, bufCell.imageAlt, cellImageMaxWidth, cellImageMaxHeight);
    }

    uint16_t contentHeight = static_cast<uint16_t>(cell.lines.size() * lineHeight);
    if (cell.image) contentHeight = static_cast<uint16_t>(contentHeight + cell.image->getRenderedHeight());
    // A cell taller than the whole viewport can never be a grid row on any device, so the
    // fragment packer would emit a row that cannot be displayed. Adopted from CrossInk,
    // which guards this case and we did not.
    if (contentHeight + 2 * TABLE_CELL_PADDING > viewportHeight) {
      LOG_DBG("EHP", "Table cell is %u px, taller than the %u px viewport — falling back to paragraphs",
              static_cast<unsigned>(contentHeight), static_cast<unsigned>(viewportHeight));
      return false;
    }
    if (contentHeight > maxContentHeight) maxContentHeight = contentHeight;
    out.cells.push_back(std::move(cell));
  }

  // Pad a short row out to the full grid so the renderer's spans always sum to columnCount.
  while (col < columnCount) {
    out.cells.emplace_back();
    ++col;
  }

  if (maxContentHeight == 0) maxContentHeight = static_cast<uint16_t>(lineHeight);
  out.height = static_cast<uint16_t>(maxContentHeight + 2 * TABLE_CELL_PADDING);
  return true;
}

void ChapterHtmlSlimParser::flushTableFragment(TableFragmentPacker& packer) {
  if (packer.rows.empty()) return;

  // When bordered: outer drawRect covers top+bottom; inter-row separators (+1 per row) are already
  // in packer.height; add 1 for the bottom border. When borderless: packer.height is exact.
  const uint16_t fragTotalHeight =
      packer.hasBorder ? static_cast<uint16_t>(packer.height + 1) : static_cast<uint16_t>(packer.height);

  if (currentPageNextY + fragTotalHeight > viewportHeight && currentPageNextY > 0) {
    emitPage();
  }

  if (auto fragment = makeUniqueNoThrow<PageTableFragment>(
          packer.cols, packer.totalWidth, fragTotalHeight, std::move(packer.rows),
          /*xPos=*/packer.xInset, /*yPos=*/static_cast<int16_t>(currentPageNextY), packer.hasBorder)) {
    noteElementOnPage(packer.firstVisibleOffset != UINT32_MAX ? packer.firstVisibleOffset : placementOffset());
    currentPage->elements.push_back(std::move(fragment));
  } else {
    LOG_ERR("EHP", "Dropping table fragment: allocation failed");
  }
  currentPageNextY += fragTotalHeight;
  packer.rows.clear();
  packer.height = 0;
  packer.cols = 0;
  packer.firstVisibleOffset = UINT32_MAX;
}

// Greedily pack laid-out rows into fragments, page-breaking between fragments. A row whose
// renderCols differs from the pending fragment flushes it first, since each PageTableFragment
// carries a single fixed column count.
bool ChapterHtmlSlimParser::emitCellAsParagraph(BufferedTableCell& cell, const bool emitImage) {
  // The cell's ParsedText is released before returning either way: this is the one place
  // that consumes a buffered cell, and holding it past layout is what made the fallback
  // path peak at buffer + emit simultaneously.
  std::unique_ptr<ParsedText> text = std::move(cell.text);

  if (text && !text->isEmpty()) {
    // Guard here rather than once per table: in streaming mode this is the only gate the
    // cells pass through, and layoutAndExtractLines below is the allocation that fails.
    if (!ensureHeapForTextLayout("table cell paragraph", text.get())) {
      return false;  // parse already stopped; cell text freed on return
    }
    auto cellBlockStyle = BlockStyle();
    cellBlockStyle.textAlignDefined = true;
    cellBlockStyle.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                   ? CssTextAlign::Justify
                                   : static_cast<CssTextAlign>(paragraphAlignment);
    // Re-use the existing paragraph pipeline by moving the cell text into currentTextBlock
    startNewTextBlock(cellBlockStyle);
    // Laid out as page paragraphs now, so wired as page paragraphs. layoutTableRow left this text
    // wired for a grid row: lines in the arena with NO page-fit hook, the row being placed whole
    // inside a block of its own. Kept like that here, a line that did not fit was materialised in
    // the current page's block BEFORE addLineToPage emitted that page -- and the emit rewound the
    // block under it. The line went on to the next page pointing at reclaimed bytes, the lines
    // after it overwrote them, and the page was written with a corrupt TextBlock ("corrupt word
    // offset", the page unreadable even after a rebuild). Seen on an X3 in Alice's chapter 2,
    // where a cell holding a tall picture fell back to paragraphs. Arena builds only.
    wireParagraphLines(*text);
    // Transfer words from the buffered cell text into the new currentTextBlock
    // by re-running layout directly
    text->layoutAndExtractLines(
        renderer, fontId, viewportWidth,
        [this, &text](std::unique_ptr<TextBlock> tb, bool lineEndsWithHyphen, bool suppressRetry) {
          return addLineToPage(std::move(tb), lineEndsWithHyphen, suppressRetry, text->lastLineVisibleOffset());
        });
  }
  text.reset();  // free the words before the image decode below needs contiguous heap

  // degradeRow() emits images in a second pass (emitRowImagesAsBlocks) to keep all of the row's
  // text above all of its images, matching the historical fallback order. A cell closed while
  // already degraded has no second pass, so it emits its image inline here.
  if (emitImage && !cell.imageSrc.empty()) {
    placeImageBlockAsBlock(buildCellImage(cell.imageSrc, cell.imageAlt, viewportWidth, viewportHeight));
    cell.imageSrc.clear();
    cell.imageSrc.shrink_to_fit();
    cell.imageAlt.clear();
    cell.imageAlt.shrink_to_fit();
  }
  return true;
}

bool ChapterHtmlSlimParser::emitRowAsParagraphs(BufferedTableRow& row) {
  // Emit each cell as a sequential paragraph (content-preserving fallback).
  // Each cell's ParsedText is released as it is laid out (see emitCellAsParagraph), so the
  // buffer shrinks while the emit grows instead of both peaking together. The cell shells stay
  // until emitRowImagesAsBlocks has run — it still needs imageSrc — and are dropped by the caller.
  for (auto& cell : row.cells) {
    if (!emitCellAsParagraph(cell, /*emitImage=*/false)) {
      // Heap guard stopped the parse. Drop the remaining buffered text now rather than holding it
      // until the row is destroyed; nothing further will be laid out.
      for (auto& c : row.cells) c.text.reset();
      return false;
    }
  }
  return true;
}
