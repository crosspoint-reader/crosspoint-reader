#define DEBUG_MEMORY_CONSUMPTION 1
#define DEBUG_BACKGROUND_WORK 1
#define DEBUG_BACKGROUND_OVERLAY 0

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

// When 1, draws a small overlay in the status bar showing background-work progress:
//   A<.|x>  next-page pre-render: '.' running/scheduled, 'x' ready
//   B<nn%>  next-section background build percent (omitted when inactive)
// and dumps A/B run+complete counters to serial every ~5s. Diagnostic aid for the
// Background A/B work; compiled out (zero cost) when 0.
#ifndef DEBUG_BACKGROUND_WORK
#define DEBUG_BACKGROUND_WORK 0
#endif

#include "EpubReaderActivity.h"

#include <CooperativeAbort.h>
#include <Epub/FootnotePreviews.h>
#include <Epub/FootnoteShape.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>

#include "BookRelocation.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderPrintedPageInputActivity.h"
#include "FinishedBookActivity.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "QuickOverridesActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "SerialTestApi.h"
#include "SilentRestart.h"
#include "StarredPagesActivity.h"
#include "activities/home/BookInfoActivity.h"
#include "activities/settings/DictionarySelectionActivity.h"
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/LinkMarkerMatch.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/SerialCommandGrammar.h"
#include "util/WakeTrace.h"

// Defined further down (near the other font helpers); declared here because
// buildRenderParams() above it needs the ladder.
static FontSizeLadder buildReaderFontSizeLadder(int bodyFontId);
static void buildBookFontFamilies(BlockFontFamilies& families, const std::string& cachePath, int bodyFontId,
                                  uint8_t pointSize);

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()

// Whether the status bar shows the chapter page count, as text or as a chapter progress bar.
bool chapterCounterShown() {
  return SETTINGS.statusBarChapterPageCount ||
         SETTINGS.statusBarUpperProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::CHAPTER_PROGRESS ||
         SETTINGS.statusBarLowerProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::CHAPTER_PROGRESS;
}

// Human-readable effective refresh mode for the page-summary diagnostic log.
const char* refreshModeName(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "full";
    case HalDisplay::HALF_REFRESH:
      return "half";
    case HalDisplay::FAST_REFRESH:
      return "fast";
    default:
      return "?";
  }
}

// Parse a printed-page label as a non-negative integer. Returns nullopt for empty strings,
// strings with non-digit characters (e.g. roman "iv"), and overflow. Used both to gate the
// "go to printed page" menu item and to compute min/max for the numeric input.
std::optional<int> parsePrintedPageLabel(const std::string& label) {
  if (label.empty()) return std::nullopt;
  int value = 0;
  for (char c : label) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
    if (value > 999999) return std::nullopt;  // sanity
  }
  return value;
}
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};

// Pre-render of the next page within the current chapter only runs when heap is healthy.
// 44 KB, derived from what the pass actually consumes rather than from what happens to be
// free — this floor was set the other way twice and was wrong both times:
//   2026-06-11 (X3): 64 KB silently disabled Background A whenever steady free dipped to
//     ~65 KB (e.g. right after a cache rebuild), which also starved Background B behind it.
//     Lowered to 56 KB.
//   2026-08-02 (X3): the scaled-glyph mask cache (~4.75 KB, see GfxRenderer) moved steady
//     free down ~5 KB, so even the post-AA peak (~54.9 KB) fell under 56 KB and A stopped
//     running for entire sessions — silently, which is why the skips below now log.
// Measured 2026-08-02 (X3, prerender_begin/after_load/end snapshots, 4 consecutive pages):
//   Page::deserialize      10504-10636 B  (the dominant cost, and the only one that had
//                                          never been measured)
//   prewarm buffers        -448..+1872 B  (net ~neutral: replaces the previous page's)
//   deepest transient      peakTemp, up to 8467 B on this font, inside prewarm decompress
//   => ~23 KB of headroom below the entry point, nothing retained afterwards (the Page dies
//      with the pass; only framebuffer pixels survive).
// 44 KB therefore troughs at ~21 KB worst case, above the ~16 KB the sliced section build
// reserves, and leaves X3 (which enters at 53-55 KB) ~10 KB of margin instead of ~2. The
// transient is also held under RenderLock, so no other reader work can allocate into the dip.
constexpr uint32_t PRE_RENDER_MIN_FREE_HEAP_BYTES = 44 * 1024;

// Background B (next-section pre-build) builds ONLY inside the borrowed secondary framebuffer.
// It used to carry a second, heap-backed admission path (BG_BUILD_PARSE_MIN_FREE / EXTRACT_BASE /
// MIN_CONTIG and a 72 KB CSS floor, derived from a 40 KB resolver floor no build has used since
// every build went lean) for the case where there is no buffer to lend. The HEAP_GATE trace never
// saw those gates admit on any spine of any book on either device -- ~57 KB free / ~25 KB contig
// is the reading steady state and the floors started at 63 KB / 41 KB -- so the path was dead
// code that cost a central-directory scan per target spine. Deleted (memory audit 2026-09, R3):
// with no buffer to lend, B waits; Background-C builds the section on navigation.
//
// Floors for the BORROWED-buffer B build (beginBackgroundBorrow). The gates above size a build
// that allocates from the heap; a borrowed build does not — its parse working set, inflate ring
// and CSS index all bump-allocate inside the lent ~48 KB region (see Section::runBuildParse's
// sharedZipScope and runBuildSetup's setIndexArena/setLeanResolve). What still comes from the
// heap is the small fixed setup (parser object, file handles, std::string paths) plus whatever
// the CSS resolver needs above the LEAN floor it drops to in arena mode. These floors cover that
// remainder with reserve, and are reachable from the ~57 KB reading steady state.
//
// Re-derived 2026-09-26 from device measurements (memory audit 2026-09, section 8; X3 runs 18-20):
//  - the reading state is no longer ~57 KB: the free heap B sees after a page's deferred AA pass
//    has a median of ~37 KB, a 25th percentile of 34-36 KB and a floor of ~32.5 KB. 40 KB was out
//    of reach on the X3, so B never ran at all (run 18: waited in WaitHeap for a whole chapter);
//  - a borrowed build's heap cost, from its start to its lowest per-page reading, is 22.6 KB on
//    the heaviest chapter measured (Chapter 3 of Strange Pictures, 27 images) and 16.2 KB on a
//    text chapter. Since the image-header probe went into the arena (5ce8564ee) nothing larger
//    than that happens between two readings;
//  - the parser aborts below MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD (9 KB), and B discards an aborted
//    build (three in a row switch B off for the book).
// 35 KB keeps the worst chapter's low at ~12.4 KB, 3 KB clear of the abort, and is met in most
// quiet moments. A render never overlaps B's heap: it takes the buffer back and the live build
// is discarded first (recoverSecondaryBufferIfNeeded).
//
// Now 14 KB (2026-10-01): a floor that predicts a whole build's cost will always be a guess, and a
// cautious one keeps B from running at all -- on the X3 it read 31-36 KB and stayed in WaitHeap for
// every chapter of a book. A build already protects itself while it runs, as the foreground build
// does at whatever heap the reader has: the parser stops below MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD
// (9 KB) and leaves a truncated section, CSS lookups degrade instead of failing, an image header
// that does not fit is refused -- and B discards every such result, skips that chapter and pauses
// until the next one (backgroundPausedForChapter_). So the floor only has to get a build STARTED
// before those checks take over: 9 KB of cushion, the ~2 KB a build's setup takes (X3, Venus in
// Copper: start 49 276 -> setup done ~47 490, extraction another ~0.2 KB) and ~3 KB of margin.
// What a build really cost is logged when it hands the buffer back (endBackgroundBorrow).
#ifndef BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES
#define BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES (14 * 1024)
#endif
#ifndef BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES
#define BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES (12 * 1024)
#endif
// The allocator reports its largest free block a few bytes under the round number (the block
// header), so a contig floor sitting exactly on a power of two is refused by a heap that has the
// memory -- the parser file documents the same 12-bytes-short refusals. Every contig floor in
// this file whose refusal changes the build path subtracts it.
constexpr uint32_t LARGEST_FREE_BLOCK_SLACK = 16;
// Added to the free-heap floor (either path) for a target that still owes the inline-footnote
// resolve. That pass holds a SAX parser (~9 KB), a 1 KB stream chunk and the store's index on the
// HEAP for as long as it runs — and because it runs in slices, that is across every page render
// in between, not for one transient moment. (Its one big allocation, the inflate ring for an
// un-banked note document, comes out of the build's arena instead; see DocStream::open.)
// Deliberately smaller than the ~14 KB the pass wants: the floors above already carry reserve,
// and the reading steady state is ~51 KB free (device trace, issue #211), so a bigger number
// would push the gate out of reach and re-lose the look-ahead it exists to protect. A resolve
// that still cannot fit fails cleanly — the build is discarded and the foreground rebuilds the
// spine released, where it has ~52 KB more to work with.
//
// Zero since 5ce8564ee (memory audit 2026-09, section 8): the pass's SAX state (9 832 B) now comes
// from the build's arena, which holds ~4 KB while the pass runs, and what stays on the heap (a
// 1 KB chunk, the archive reader, the target list) is taken while the build holds only its setup
// heap -- well under the layout peak the free floor above is derived from. Kept as a knob.
#ifndef BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES
#define BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES 0
#endif

// Quiet period after the last page reached the screen before B may take the buffer. B's borrow
// costs a page's AA if the reader turns during it, and a preempted slice is wasted work — so
// only start once the reader has settled. Below this the reader is flipping (skimming, or
// holding the button), which is when B should stay out of the way.
//
// 4000 -> 1500 (2026-08-11), because the two things that made 4000 conservative are both gone:
//   - a preempted attempt used to throw away its inflated XHTML, so a retry re-paid the whole
//     extraction. It now keeps it (Section::abortSectionBuild keys on extractDone), which is
//     what makes attempt 2 cheap.
//   - B used to start with a button edge already queued and hand the buffer straight back. It
//     now consults CooperativeAbort before taking it.
//
// Sized against what B actually needs rather than what feels safe. Measured X3, alice spine 1
// (16340 bytes, 17 pages): attempt 1 is 1652 ms total, attempt 2 skips the extract and is
// setup 123 + parse 818 = ~950 ms. 1500 clears that with margin. The old 4000 asked for ~4x
// the stillness B needs to finish, which during genuine reading (20-60 s per page) it got
// trivially, but while skimming — when chapters are crossed fastest and lookahead matters most
// — it never got at all: observed turn intervals were 1.2-5.6 s.
//
// The signal that this is wrong is `preempt=` in the BG work line. It was 0 for a whole session
// after the two fixes above; if it starts climbing toward BG_BUILD_MAX_PREEMPTIONS, B is losing
// races again and this is the number to raise.
#ifndef BG_BUILD_BORROW_QUIET_MS
#define BG_BUILD_BORROW_QUIET_MS 1500UL
#endif

// Build-through: on the last pages of a chapter, B's build of the NEXT chapter keeps the borrowed
// buffer across page turns instead of being discarded by each one, and it starts without waiting
// for BG_BUILD_BORROW_QUIET_MS. A reader moving fast enough never leaves a quiet window -- X3,
// 2026-10-01: turns 1-2 s apart through a 26-page chapter, B waited 10 s without starting and the
// next chapter opened on "Indexing". A text chapter builds in 0.4-1 s of slices, which fits into
// the waveforms of the few turns left.
//
// The price is paid on those pages only, and only while the build runs: they draw in BW with the
// normal refresh, without anti-aliasing or a pre-render of the page after. At that pace the AA was
// already being lost ("AA replay skipped: page preempted by navigation"). If the reader reaches
// the next chapter first, Background-C adopts the live build (see buildSection).
//
// X3 only: its controller keeps the displayed frame as the diff baseline, so a FAST refresh needs
// no host-side copy of the previous frame. The X4 would first need its RED RAM seeded from the
// frame on screen, which the buffer B borrows may no longer hold.
#ifndef BG_BUILD_THROUGH_PAGES
#define BG_BUILD_THROUGH_PAGES 3
#endif

// The image lane's settle after a page turn, a draw or a preempted decode (see
// stepImageWarmLocked). Short on purpose: the lane also waits for any gesture in flight and for
// the deferred AA pass, which are the real hazards; this only keeps a fast flipper from being
// handed a decode to abort on every page.
#ifndef IMAGE_LANE_SETTLE_MS
#define IMAGE_LANE_SETTLE_MS 300UL
#endif
// See backgroundPreemptCount_: give up on a target after this many preempted attempts. Two is
// deliberate — attempt 1 banks the inflated XHTML in the book-keyed HTML cache (kept on abort,
// see Section::abortSectionBuild), so attempt 2 skips inflation and is the one that gets a fair
// shot at the parse. A third would just repeat attempt 2's losing race.
#ifndef BG_BUILD_MAX_PREEMPTIONS
#define BG_BUILD_MAX_PREEMPTIONS 2
#endif
// Per-slice time budget for a Background-B parse step. Conservative start (handoff plan
// suggests 30–50 ms); tune from the DEBUG_BACKGROUND_WORK serial counters.
constexpr uint32_t BG_BUILD_BUDGET_MS = 40;

// Background-B keeps a page-budgeted window of layout ready ahead of the reading position rather
// than a fixed number of sections: it pre-builds whole subsequent spines until roughly this many
// pages of runway exist (counting the current section's unread tail plus the subsequent sections
// built so far), then idles until the reader advances and the window re-anchors. A page budget
// spans spine boundaries naturally — front matter of many tiny one-page spines gets several built,
// while one big chapter already covers the budget on its own. Bounds continuous CPU/SD cost on a
// battery e-reader (vs. indexing the whole book up front).
//
// Adapted from crosspoint-reader's BUILD_WINDOW_AHEAD (PR #2452 by GitHub user itsthisjustin,
// "Lazy incremental EPUB section indexing"); here the window is a page budget that spans spines
// rather than a per-section count.
#ifndef BG_BUILD_LOOKAHEAD_PAGES
#define BG_BUILD_LOOKAHEAD_PAGES 50
#endif

// Foreground in-place section build (the "keep the secondary buffer" path). When heap is
// ample we build the new section WITHOUT releasing the secondary framebuffer, so the chapter's
// first page keeps a valid fast-refresh baseline and avoids the baseline-resetting half-
// refresh. Conservative floors: the foreground build runs at page-turn time with the secondary
// buffer (and possibly a pre-rendered page) resident, so pin above Background-B's idle gate.
// Failure is recoverable — the build retries with the buffer released — so these gate "try in
// place" rather than guaranteeing success; tune from the "Index start mem" serial logs.
//
// SHAPE (fixed 2026-08-15, device-measured — see heapAllowsInPlaceBuild): the free floor is the
// MAXIMUM of the two phase peaks, not their sum. runBuildParse releases the inflate ring before
// the parse begins, so the ring and the layout working set are never live together; adding them
// asked for memory no build has ever needed. The bases below are the PARSE-phase floors and are
// unchanged by that fix — only the arithmetic combining them with the ring changed, so an A/B
// against the old behaviour reads cleanly.
#ifndef IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES (60 * 1024)
#endif
#ifndef IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES (28 * 1024)
#endif
// Extract-phase free floor, to which the entry's ring is added (the ring IS live in this phase,
// so here the sum is correct). Its own name: this floor is reached from a page turn, not from
// Background-B's borrow-first path (whose heap-backed twin was deleted in the 2026-09 audit).
//
// 30 -> 50 KB (2026-09-01), device-measured on Small Gods spine 1 — the whole book in one
// 583991-byte entry — which the 30 KB version admitted to a resident build that could not
// possibly finish. The trace, from the gate to the abort ~170 ms later:
//
//   gate      free=71592  (floor was 67584: the CSS base, since 30720 + 32768 = 63488 lost the max)
//   start     free=60396  -11196  activity/section/popup before the build begins
//   setup     free=56648   -3748  CSS index load
//   abort     free=19924  -36724  ZipFile + read buffer + the 32768 inflate ring
//
// So the ring is only two thirds of what the extract phase costs from here; the other ~18.9 KB is
// fixed overhead the base is meant to cover, and on top of that the build has to stay above
// RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES (30 KB) or it aborts anyway. 18.9 + 30 = 48.9 KB, so 50
// with a little margin. Note what this does NOT change: with the ring capped at 32 KB the base
// only binds for large entries — a few-KB chapter still floors at the flat 60/66 KB above and
// still builds in place. What it changes is that a whole-book-in-one-spine entry now goes
// straight to the released path instead of paying ~1.4 s for setup, abort, cache clear and a
// second setup to get there.
#ifndef IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES
#define IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES (50 * 1024)
#endif
// CSS books need more margin to build in place: the parse resolves embedded styles, and the lean
// resolver skips lookups below its floor, which yields a css-degraded cache and (since audit R3
// step 4) one released rebuild -- seconds of resident work thrown away. Since every build is
// two-phase (the inflate ring is released BEFORE the CSS-resolving parse), the resolve runs with
// the ring gone; contig stays pinned at the ring size (<=32 KB) for the extraction phase.
//
// Re-derived 2026-09-26 (memory audit R3). The old 66 KB was "parse working set ~25-28 KB + the
// 40 KB resolver floor"; the resolver has run lean on every build since Section::runBuildSetup
// set it unconditionally, so the floor it protects is CSS_LEAN_MIN_FREE_HEAP_FOR_CSS:
//   parse working set of a heap-backed CSS build (SAX ~10 KB, index, page/paragraph heap)  28 KB
//   resolver lean floor                                                                   24 KB
//   margin                                                                                 4 KB
//                                                                                       = 56 KB
// The residentAbort guard (RESIDENT_BUILD_ABORT_*) still catches a build that dips further. X4
// device confirmation of the new admission band (56-66 KB free) is pending.
#ifndef IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES (56 * 1024)
#endif
#ifndef IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES
#define IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES (32 * 1024)
#endif
// Proactive low-heap guard for a resident (AA-buffer-kept) Background-C build. The in-place start
// floors can't bound the parse's transient working set, which can ride free heap well down on a
// big chapter (a 123 KB CSS section was observed dipping to ~24 KB). If the between-slice baseline
// falls below these, abandon the resident build and rebuild on the released path (frees the
// ~48 KB buffer) before an allocation fails. Pinned below typical mid-build baselines so a build
// that is coping is not aborted, but above the ~13-15 KB zone where heap-pressure faults appear.
#ifndef RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES
#define RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES (30 * 1024)
#endif
#ifndef RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES
#define RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES (16 * 1024)
#endif

// The link Back stack's file in the book's cache directory, beside progress.bin (EpubLinkBackStack).
constexpr const char* LINK_STACK_FILE = "/linkstack.bin";

constexpr uint8_t TRUNCATED_SECTION_HINT_RENDER_COUNT = 2;
constexpr const char* TRUNCATED_SECTION_HINT_LINE_1 = "Chapter may be truncated (low memory).";
constexpr const char* TRUNCATED_SECTION_HINT_LINE_2 = "Try: No embedded style | No images | AA Off";

// --- Heap-gate instrumentation (temporary; heap/gate-rederivation branch) --------------------
// The reader path has ~8 free/contig floors (PRE_RENDER_, BG_BUILD_*, IN_PLACE_BUILD_*,
// RESIDENT_BUILD_ABORT_*) that predate the build arena. Several of them REJECT SILENTLY, so a
// device trace cannot show which floor sent a build down the released path — and the released
// path is where the X3 fragmented-heap restart happens. Device-measured example (X4):
//
//   heapAllowsInPlaceBuild=0 css=1 ring=15799 free=71544(floor=83383) contig=59380(floor=32768)
//
// contig passes with 26 KB to spare; `free` fails against a floor that is UNREACHABLE (post-font
// free peaks at ~97 KB and is ~71 KB by reader time), because the floor added ringBytes on top of
// a base already sized for the working set. Meanwhile the arena serves every build with
// failedAlloc=0 at ~30 KB high-water. One line per gate, so one trace shows every decision and
// the arithmetic behind it.
//
// RESOLVED for that gate (2026-08-15): heapAllowsInPlaceBuild now takes max(parse, extract+ring)
// instead of parse+ring — see the derivation there, and the X4 trace that measured it.
//
// The two Background-B heap-backed gates (bgB_waitheap, bgB_cssResident) that the same trace
// showed never admitting on any spine were deleted in the 2026-09 memory audit (R3); B now builds
// only in the borrowed buffer. See the note above the BG_BUILD_BORROW_* floors.
//
// HEAP_GATE_TRACE=0 compiles it out. Default OFF (2026-08-11). It did its job twice over — the
// unreachable in-place floor above and B's contig-floor miss were both found with it — so it
// stays available rather than being deleted: build with -DHEAP_GATE_TRACE=1 when tuning a floor.
// At ~1 Hz per waiting gate it is far too loud to leave on.
#ifndef HEAP_GATE_TRACE
#define HEAP_GATE_TRACE 0
#endif

#if HEAP_GATE_TRACE
// `floor` of 0 means "not a numeric floor" (e.g. a delegated predicate); printed as `-`.
void logHeapGate(const char* gate, const bool passed, const uint32_t freeHeap, const uint32_t freeFloor,
                 const uint32_t contigHeap, const uint32_t contigFloor) {
  LOG_INF("HEAP", "gate=%-22s %s free=%lu(floor=%lu) contig=%lu(floor=%lu)", gate, passed ? "PASS" : "REJECT",
          static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(freeFloor),
          static_cast<unsigned long>(contigHeap), static_cast<unsigned long>(contigFloor));
}
#define HEAP_GATE(gate, passed, f, ff, c, cf) logHeapGate((gate), (passed), (f), (ff), (c), (cf))
#else
#define HEAP_GATE(gate, passed, f, ff, c, cf) ((void)0)
#endif

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Temporary corruption tripwire: walks the entire heap and names the checkpoint that
// sees damage. DANGEROUS on this platform: the ESP32-C3 startup-stack heap region
// ([SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE, SOC_ROM_STACK_START)) carries trampled
// TLSF metadata from boot, and tlsf_check chasing its garbage pointers crashes the
// walker (observed: Load access fault inside block_is_free on X4, decoded via
// addr2line). Only enable for dedicated diagnostics sessions, never in normal builds —
// gated on the same flag as the BootHeapProbe static-init probes.
void checkHeapIntegrity(const char* checkpoint) {
  static bool corruptSeen = false;
  if (heap_caps_check_integrity_all(true)) {
    return;
  }
  LOG_ERR("ERS", "HEAP CORRUPT at checkpoint: %s%s", checkpoint, corruptSeen ? " (repeat)" : " <-- FIRST");
  corruptSeen = true;
}
#else
inline void checkHeapIntegrity(const char*) {}
#endif

// Pin forensics for a failed secondary-framebuffer realloc: which USED blocks split the big free
// spans. heap_caps_dump() cannot be used for this -- it prints every block (hundreds of lines)
// while holding the heap lock with interrupts masked, and on X3 2026-09-25 that tripped the
// 300 ms Interrupt WDT mid-dump (Guru Meditation, reboot). The walker callback runs under the same
// lock, so it only copies into a fixed static table; the printing happens after the walk returns.
//
// Recorded: every free span >= PIN_SPAN_MIN, and every used block bordering one (the candidate
// pins), with its first words. With CONFIG_HEAP_POISONING_LIGHT the block starts with the poison
// head {0xABBA1234, requested size}; the words after it are the payload -- a vtable pointer
// (0x3cxxxxxx rodata: `nm -n firmware.elf` finds the "vtable for X" at or below it) or text
// identifies the owner. A payload of two heap pointers is usually an unwritten block's stale
// TLSF free-list links.
struct HeapPinEntry {
  uintptr_t addr;
  uint32_t size;
  bool used;
  uint32_t words[4];
};
struct HeapPinWalk {
  static constexpr size_t MAX_ENTRIES = 24;
  static constexpr size_t PIN_SPAN_MIN = 1024;
  HeapPinEntry entries[MAX_ENTRIES];
  size_t count = 0;
  size_t dropped = 0;
  HeapPinEntry prev{};  // previous block of the current heap
  bool prevValid = false;
  bool prevRecorded = false;
  intptr_t heapStart = 0;

  void record(const HeapPinEntry& e) {
    if (count < MAX_ENTRIES) {
      entries[count++] = e;
    } else {
      ++dropped;
    }
  }
};

HeapPinEntry makePinEntry(const walker_block_info_t& block) {
  HeapPinEntry e{reinterpret_cast<uintptr_t>(block.ptr), static_cast<uint32_t>(block.size), block.used, {}};
  if (block.used && block.size >= sizeof(e.words)) memcpy(e.words, block.ptr, sizeof(e.words));
  return e;
}

bool heapPinWalker(walker_heap_into_t heap, walker_block_info_t block, void* user) {
  auto& w = *static_cast<HeapPinWalk*>(user);
  if (heap.start != w.heapStart) {  // new heap: adjacency does not cross heap boundaries
    w.heapStart = heap.start;
    w.prevValid = false;
  }
  const HeapPinEntry cur = makePinEntry(block);
  const bool curBigFree = !cur.used && cur.size >= HeapPinWalk::PIN_SPAN_MIN;
  const bool prevBigFree = w.prevValid && !w.prev.used && w.prev.size >= HeapPinWalk::PIN_SPAN_MIN;
  bool curRecorded = false;
  if (curBigFree) {
    if (w.prevValid && w.prev.used && !w.prevRecorded) w.record(w.prev);  // used block before the span
    w.record(cur);
    curRecorded = true;
  } else if (cur.used && prevBigFree) {
    w.record(cur);  // used block after the span
    curRecorded = true;
  }
  w.prev = cur;
  w.prevValid = true;
  w.prevRecorded = curRecorded;
  return true;
}

void logHeapPinForensics() {
  // ~700 B: too big for the render-task stack, and not worth permanent BSS for a once-per-boot
  // diagnostic. Allocated before the walk and freed after, so it cannot be a pin it reports.
  auto walkOwner = makeUniqueNoThrow<HeapPinWalk>();
  if (!walkOwner) {
    LOG_ERR("ERS", "  (no heap for the pin walk)");
    return;
  }
  HeapPinWalk& walk = *walkOwner;
  heap_caps_walk(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT, heapPinWalker, &walk);
  for (size_t i = 0; i < walk.count; ++i) {
    const HeapPinEntry& e = walk.entries[i];
    if (e.used) {
      LOG_ERR("ERS", "  pin  0x%08x size=%5lu words=%08lx %08lx %08lx %08lx", static_cast<unsigned>(e.addr),
              static_cast<unsigned long>(e.size), static_cast<unsigned long>(e.words[0]),
              static_cast<unsigned long>(e.words[1]), static_cast<unsigned long>(e.words[2]),
              static_cast<unsigned long>(e.words[3]));
    } else {
      LOG_ERR("ERS", "  free 0x%08x size=%5lu", static_cast<unsigned>(e.addr), static_cast<unsigned long>(e.size));
    }
  }
  if (walk.dropped != 0) LOG_ERR("ERS", "  (+%u entries not recorded)", static_cast<unsigned>(walk.dropped));
}

#if DEBUG_MEMORY_CONSUMPTION
// The `Min Free` in the periodic [MEM] line is esp_get_minimum_free_heap_size() — a boot-wide
// watermark with no timestamp, so it tells you the session dipped to N and nothing about where.
// X3 2026-08-11 reported 7772 (against 23960 the run before, and a documented fault zone of
// ~13-15 KB) while the lowest value any probe actually logged was 23028, so the dip is happening
// between the points that sample.
//
// A watermark only ever falls, so noticing the fall is enough: sample it wherever we already
// sample the heap and report only when it MOVED, naming the interval it moved in. The label is
// the stage the drop was DETECTED at, i.e. the dip happened somewhere between the previous
// snapshot and this one — that is the bracket, not a point measurement.
uint32_t g_lastHeapWatermark = 0;

// The watermark of the pool that can actually run out on THIS board.
//
// esp_get_minimum_free_heap_size() sums every heap the allocator owns. On the
// C3 that is the one pool the whole hunt above was about, so it is exactly
// right. On a PSRAM board it also sums in 8 MB of SPI RAM, and the number stops
// meaning anything: the X4 Pro reported drops of 152, 296 and 656 bytes against
// an 8.4 MB total -- noise at ERROR level, several lines per page turn, in a
// diagnostic whose entire job is to make a real dip stand out.
//
// Internal RAM is the pool under pressure there (DMA buffers, task stacks, and
// everything under the 4 KB CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL threshold), and
// it is what the periodic [MEM] line already reports as `internal=`. So watch
// that instead. On a board without PSRAM the two are the same pool and the C3
// behaviour is unchanged -- which is the point: this narrows the question to the
// scarce resource rather than muting the alarm.
uint32_t scarceHeapWatermark() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
#else
  return esp_get_minimum_free_heap_size();
#endif
}

void noteHeapWatermark(const char* stage) {
  const uint32_t mark = scarceHeapWatermark();
  if (g_lastHeapWatermark == 0) {
    g_lastHeapWatermark = mark;  // first call: establish the baseline, nothing to report yet
    return;
  }
  if (mark >= g_lastHeapWatermark) return;
  LOG_ERR("MEM", "Watermark DROP %lu -> %lu (-%lu) in the interval ending at [%s]",
          static_cast<unsigned long>(g_lastHeapWatermark), static_cast<unsigned long>(mark),
          static_cast<unsigned long>(g_lastHeapWatermark - mark), stage);
  g_lastHeapWatermark = mark;
}

void logReaderMemSnapshot(const char* stage) {
  noteHeapWatermark(stage);
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  // free+contig alone cannot tell "something is retaining bytes" from "the same bytes have
  // fragmented": both show up as contig falling. The block counts separate them --
  //   allocated rising, free bytes falling  -> a session-lifetime allocation is accumulating
  //   allocated flat, freeBlk rising        -> pure fragmentation, the bytes came back split
  // The X3 trace has contig decaying 49140 -> 38900 -> 15860 across one session with the
  // secondary buffer RELEASED at each sample, so the cause is resident, not the framebuffer.
  multi_heap_info_t info{};
  heap_caps_get_info(&info, MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu allocBlk=%lu freeBlk=%lu allocBytes=%lu", stage, freeHeap,
          contigHeap, static_cast<unsigned long>(info.allocated_blocks), static_cast<unsigned long>(info.free_blocks),
          static_cast<unsigned long>(info.total_allocated_bytes));
}
#else
inline void logReaderMemSnapshot(const char*) {}
inline void noteHeapWatermark(const char*) {}
#endif

// Computes the [0..100] EPUB progress percent. Returns 0 when pageCount is unknown (sync/bookmark
// pre-render writes), in which case the next saveProgress() will overwrite progress.bin with the
// real value before the user can leave the reader.
uint8_t epubProgressPercentByte(const Epub& epub, const int spineIndex, const int currentPage, const int pageCount) {
  if (pageCount <= 0) {
    return 0;
  }
  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  return ReaderUtils::fractionProgressPercentByte(epub.calculateProgress(spineIndex, chapterProgress));
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

int getImageOnlyPageYOffset(const Page& page, const int viewportHeight) {
  if (viewportHeight <= 0 || page.elements.empty()) {
    return 0;
  }

  const bool imageOnlyPage = std::all_of(
      page.elements.begin(), page.elements.end(),
      [](const std::unique_ptr<PageElement>& element) { return element && element->getTag() == TAG_PageImage; });
  if (!imageOnlyPage) {
    return 0;
  }

  int16_t imgX, imgY, imgW, imgH;
  if (!page.getImageBoundingBox(imgX, imgY, imgW, imgH) || imgH >= viewportHeight) {
    return 0;
  }

  const int centeredTop = (viewportHeight - imgH) / 2;
  return std::max(0, centeredTop - static_cast<int>(imgY));
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  logReaderMemSnapshot("onEnter_begin");
  WakeTrace::mark(WakeTrace::Phase::ActivityEnter);
  // Bisect anchor: corruption already present HERE means the writer ran before the
  // reader (Home sidecar JPEG conversion / thumb generation are prime suspects — see
  // the long-standing "heap may be corrupt after image decode failures" note below).
  checkHeapIntegrity("reader_onEnter");
  // Start the Background-B settle window now, so the interval is measured from entering the
  // reader rather than from millis()==0. Otherwise B's quiet check is satisfied before the book
  // has drawn anything, which is the worst possible moment to take the display buffer.
  lastPageOnScreenMs_ = millis();
  secondaryBufferDegraded_ = !renderer.hasSecondaryBuffer();
  // Cold open: arm the dramatic-transition HALF for the first section entry only (cleared by any
  // non-incremental entry in buildSection). Also clear any stale post-popup HALF left armed if the
  // previous reader session was abandoned mid-build.
  coldOpenHalfRefreshArmed_ = true;
  forceHalfRefreshAfterPopup_ = false;
  // Start the refresh cadence at the configured frequency so the first page uses a fast
  // differential. RED RAM is valid: the previous activity's last displayBuffer() called
  // syncRedRamFromFrameBuffer(). If the previous activity set a HALF_REFRESH override via
  // enforceExitFullRefresh(), consumeRefreshOverride() will honour it on the first display call.
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();

  // Drop any input events that arrived from the activity that launched us (e.g. a wake-up power
  // button hold) before they reach the page-turn handling — see ReaderUtils::InputDrainGuard.
  inputDrainGuard.arm();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }
  logReaderMemSnapshot("onEnter_after_orientation");

  epub->setupCacheDir();
  logReaderMemSnapshot("onEnter_after_setupCacheDir");

  if (getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    // Just loads images.bin (or starts an empty cache) — no whole-ZIP scan. Image
    // dimensions are resolved + cached lazily as section indexing first hits each image.
    epub->loadImageManifest();
    logReaderMemSnapshot("onEnter_after_image_manifest");
  }

  // Load the persistent baseline (progress.bin) first. Pending session state
  // (sync result, bookmark jump) is then overlaid on top — this is the only order
  // that lets a Kind::Paragraph / Kind::ListItem / Kind::VisibleOffset navTarget set by
  // applyPendingSyncSession survive into render(). The previous order (apply then load)
  // clobbered the LUT target with Kind::Page from progress.bin, which is why XPath-precision
  // sync silently degraded to the rough page estimate.
  FsFile f;
  bool hadSavedProgress = false;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[EpubProgressRecord::kMaxSize];
    const int dataSize = f.read(data, sizeof(data));
    if (const auto record = EpubProgressRecord::decode(data, dataSize > 0 ? static_cast<size_t>(dataSize) : 0)) {
      currentSpineIndex = record->spineIndex;
      // A Page target even when the paragraph is known: a Page target is shown the moment its
      // page is built (Background-C), and the paragraph only matters after a relayout.
      navTarget = NavigationTarget::makePage(record->page);
      navTarget.restoreParagraph = record->paragraph.value_or(0);
      navTarget.cachedSpineIdx = currentSpineIndex;
      navTarget.cachedPageCount = record->pageCount;
      hadSavedProgress = true;
      LOG_DBG("ERS", "Loaded cache: %d, %d (paragraph %d)", currentSpineIndex, record->page,
              record->paragraph ? *record->paragraph : -1);
    }
    f.close();
  }
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid saved spine index %d (valid 0..%d), resetting to start", currentSpineIndex,
            epub->getSpineItemsCount() > 0 ? epub->getSpineItemsCount() - 1 : 0);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
  }

  applyPendingSyncSession();
  applyPendingBookmarkJump();
#if SERIAL_TEST_API
  applyPendingPercentJump();
#endif
  logReaderMemSnapshot("onEnter_after_pending_sync");

  // True first open only: no progress.bin record. Skip the front matter to the text reference.
  // A saved position of spine 0 (reading the cover/chapter 0) must NOT be overridden — the old
  // `currentSpineIndex == 0` test couldn't tell "never opened" from "saved at chapter 0" and
  // bounced the reader to the text start on every reopen at the cover.
  if (!hadSavedProgress && currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }
  logReaderMemSnapshot("onEnter_after_progress_load");
  // Covers setupCacheDir + the image manifest + progress.bin + the sync/bookmark overlays —
  // i.e. everything needed to know WHICH page to show, before anything is read to show it.
  WakeTrace::mark(WakeTrace::Phase::ProgressLoaded);

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());
  logReaderMemSnapshot("onEnter_after_bookmarks_loaded");
  loadLinkBackStack();

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  std::string series = epub->getSeries();
  if (!series.empty() && !epub->getSeriesIndex().empty()) {
    series += " #" + epub->getSeriesIndex();
  }
  RecentBook currentBook;
  {
    // One load of the list for both: the add and the read of this book's overrides. Released
    // again before the reader allocates anything of its own.
    const RecentBooksStore::Hold recents;
    RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), series,
                         ReaderActivity::coverThumbPlaceholder(epub->getPath()));
    currentBook = RECENT_BOOKS.getBookByPath(epub->getPath());
  }
  bookEmbeddedStyleOverride = currentBook.embeddedStyleOverride;
  bookImageRenderingOverride = currentBook.imageRenderingOverride;
  bookFontFamilyOverride = currentBook.fontFamilyOverride;
  bookSdFontFamilyOverride = currentBook.sdFontFamilyOverride;
  bookFontSizeOverride = currentBook.fontSizeOverride;
  bookBionicReadingOverride = currentBook.bionicReadingOverride;
  bookParagraphAlignmentOverride = currentBook.paragraphAlignmentOverride;
  bookTextAntiAliasingOverride = currentBook.textAntiAliasingOverride;
  bookHyphenationOverride = currentBook.hyphenationOverride;
  bookFontSizeNormalizationOverride = currentBook.fontSizeNormalizationOverride;
  bookGuideDotsOverride = currentBook.guideDotsOverride;
  bookInlineFootnotePreviewsOverride = currentBook.inlineFootnotePreviewsOverride;
  logReaderMemSnapshot("onEnter_after_recent_books");

  // Take the scaled-glyph cache now, before any page is laid out. It is never released, and
  // allocating it on first use meant a permanent block landed wherever the first heading happened
  // to be drawn -- measured on X3 as 5120 of contig lost inside a mid-build page draw, for the
  // rest of the session. Here it sits with the other permanent allocations. Sized for the body
  // font this book reads in, now that its overrides are known: ~4.9 KB at a real size, ~9.5 KB
  // when the size is synthesised. A size changed mid-book keeps the cache it has until the next
  // book opens -- more resampling there, never a wrong glyph.
  renderer.ensureScaledGlyphCache(getEffectiveReaderFontId());
  logReaderMemSnapshot("onEnter_after_glyph_cache");

  // Start a reading-stats session. We use the cheap filename-based hash here:
  // computing the content hash would re-read the file on every reader open,
  // and a renamed book getting a new stats entry is acceptable — it'll still
  // accumulate going forward.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(epub->getPath()), epub->getTitle(),
                                      epub->getAuthor());
  // Bookmarks + recent-books overrides + the stats session. These are the loads a wake
  // shortcut would most plausibly skip or cache in RTC, so they get their own bucket.
  WakeTrace::mark(WakeTrace::Phase::StoresLoaded);

#if CROSSPOINT_KOREADER_AUTOSYNC
  if (KOReaderAutoSync::sleepPushEnabled() || KOReaderAutoSync::intervalPushEnabled()) {
    AUTOSYNC_STATE.seedLastSeen(
        currentSpineIndex, navTarget.kind == NavigationTarget::Kind::Page ? navTarget.page : navTarget.fallbackPage);
  }
  autoSyncPullPending = KOReaderAutoSync::pullEnabled() && WakeTrace::isResume();
  autoSyncIndicator = SyncIndicator::None;
  if (KOREADER_STORE.getShowSyncIndicator() && AUTOSYNC_STATE.lastSyncFailed()) {
    autoSyncIndicator = SyncIndicator::Failed;
  }
#endif  // CROSSPOINT_KOREADER_AUTOSYNC

  // Trigger first update
  logReaderMemSnapshot("onEnter_before_request_update");
  requestUpdate();
  logReaderMemSnapshot("onEnter_ready");
  checkHeapIntegrity("reader_onEnter_ready");
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  logReaderMemSnapshot("onExit_before_release");

  // Leave the panel a clean baseline for whatever screen comes next.
  //
  // Every other reader (Txt, Md, Xtc, the BMP viewer) has always done this; the
  // EPUB reader did not, and got away with it because the two-push AA ended by
  // reseeding the differential baseline (cleanupGrayscaleWithPreviousBuffer).
  // The single push deliberately does no such cleanup — the panel canvas holding
  // the finished page IS the next baseline — so the greys are still on the glass
  // when the reader closes, and the home screen's FAST diff drives them from a
  // level the differential bank's columns do not start from. That showed as
  // heavy ghosting across Home and Settings.
  //
  // Only when anti-aliasing actually ran: a plain B/W page leaves the panel on
  // its rails already, and a clean-bank refresh costs about a second and a half.
  //
  // ...except where that premise does not hold. It is a statement about the X3/X4
  // controllers, whose B/W path drives to the rails. On the LGFX panels -- the ones that
  // answer supportsGrayFrame(), i.e. the T5 S3 -- EVERY push goes through the same graded
  // canvas, and FAST maps to a differential bank that deliberately skips the eraser
  // (LgfxEpdDriver::epdModeFor). So a B/W page leaves the canvas holding the page just as a
  // grey one does, and the home screen's FAST diff runs against it. Reported from hardware
  // as the last reader page and the home screen superimposed, settling a refresh later.
  if (getEffectiveTextAntiAliasing() || renderer.supportsGrayFrame()) {
    ReaderUtils::enforceExitFullRefresh(renderer);
  }

#if CROSSPOINT_KOREADER_AUTOSYNC
  maybeAutoPushOnSleep();
  releaseAutoSyncSlot();
#endif  // CROSSPOINT_KOREADER_AUTOSYNC

  // If a pre-render left the next page in the frame buffer, redraw the current page so the
  // next activity (notably SleepActivity's OVERLAY mode) sees what the user was looking at.
  // Must run before section.reset() and the orientation reset below.
  restoreCurrentPageToBufferIfPreRendered();

  // Record the paragraph under the saved page, so a reopen under a different layout lands on it
  // (EpubProgressRecord), and what the status bar shows for that page -- its place in the whole
  // chapter and its printed page -- for the sleep screen's overlay, which runs right after this and
  // cannot lay the book out to work them out. Here, not per page turn: these lookups read the
  // section cache and the page index, and the per-page saves only need to survive a reboot, which
  // comes back to the same layout. Past the last spine the finished-book flow has written its own
  // 100% record; leave that alone.
  if (section && readerPhase_ == ReaderPhase::READING && !section->hasActiveBuild() && section->pageCount > 0 && epub &&
      currentSpineIndex < epub->getSpineItemsCount()) {
    const auto page = static_cast<uint16_t>(section->currentPage);
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount, section->getParagraphIndexForPage(page),
                 shownProgress());
  }

  // Save bookmarks before exit
  bookmarkStore.save();
  saveLinkBackStack();
  if (epub) {
    GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(), false);
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  // Release any deferred AA page before tearing down the section/epub.
  pendingGrayscale_ = {};
  // Abort any in-flight Background-B build (deletes its partial cache file) before the
  // epub it references goes away.
  resetBackgroundBuild();
  section.reset();  // also aborts an in-flight Background-C build of the current section
  // Background-C may have BORROWED the secondary buffer for headroom (lent, not freed); the
  // build is now aborted (section.reset above released into the arena), so hand the block back.
  // The return cannot fail — the region never entered the heap.
  if (secondaryBorrowed_) {
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();
    secondaryBorrowed_ = false;
    secondaryBufferDegraded_ = false;
    LOG_INF("ERS", "onExit: returned secondary buffer borrowed by Background-C");
  } else if (secondaryBufferDegraded_ && !renderer.hasSecondaryBuffer()) {
    // Background-C may INSTEAD have RELEASED the secondary buffer for headroom; the build is now
    // aborted, so restore the global "buffer resident" invariant before the next activity renders.
    // `else if` because borrowed and released are alternatives, never both: returning a borrow
    // above already makes the buffer resident. That used to be expressed by the borrow branch
    // clearing secondaryBufferDegraded_, which left this branch's reachability depending on a
    // flag write two lines up rather than on the state it actually tests.
    if (reallocSecondaryEvictingCaches()) {
      LOG_INF("ERS", "onExit: restored secondary buffer released by Background-C");
    }
    secondaryBufferDegraded_ = false;
  }
  // Restore the display-global single-buffer fast-diff flag unconditionally. Background-C
  // (buildSection) sets it true alongside its release; the two designed restore sites
  // (recoverSecondaryBufferIfNeeded, compileSectionCache) clear it, but exiting mid-build
  // reaches neither. The flag lives on the shared EInkDisplay and outlives this activity,
  // so a stale true would make the next activity's first FAST refresh diff against the
  // controller's retained RED RAM instead of its host baseline (ghosting). No-op on X3.
  renderer.setSingleBufferFastDiff(false);
  UITheme::getInstance().getMutableTheme().onBookWillClose(epub ? epub->getPath() : "", epub.get(), nullptr, nullptr);
  epub.reset();
  currentPageFootnotes.clear();
  currentPageFootnotes.shrink_to_fit();

  // The glyph caches go back before the stats flush below, which wants the heap, and so Home and
  // Settings do not run 7-12 KB short after every book. The next onEnter takes them again, still
  // at a stable point rather than mid-build (GfxRenderer::ensureScaledGlyphCache).
  renderer.releaseGlyphCaches();

  // Flush the reading-stats session LAST: end() writes the book and the global figures in place
  // (~10 KB of heap whatever the history holds), and it needs nothing of the reader (the tracker
  // copied the id, title and author at begin()). Here it has the heap the teardown just freed.
  // Sleep paths that bypass onExit() still end up here on resume because the activity is
  // recreated.
  globalReadingSessionTracker().end();

  // Last: everything above saved under the book's path, which the finished-book flow may have moved.
  BookRelocation::bookClosed();
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Debug-only: periodic serial dump of background-work counters (no-op in release).
  serviceBackgroundDebugLog();

  if (pendingProgressSave.pending.load(std::memory_order_acquire)) {
    pendingProgressSave.pending.store(false, std::memory_order_relaxed);
    saveProgress(pendingProgressSave.spineIndex, pendingProgressSave.page, pendingProgressSave.pageCount);
  }

  // Deferred by the render task (renderFinishedBookPass) because it ends in pushActivity().
  // After the progress flush and before input handling: this is the reader's LAST loop() pass —
  // the launch pushes it onto the activity stack, which stops loop() being called — so anything
  // still owed must be flushed above, and nothing below it will run again.
  if (finishedBookLaunchPending_.load(std::memory_order_acquire)) {
    serviceFinishedBookLaunch();
    return;
  }

#if CROSSPOINT_KOREADER_AUTOSYNC
  if (autoSyncPullDialogLaunched) {
    return;
  }
  serviceAutoSync();
  if (autoSyncPullDialogLaunched) {
    return;
  }
#endif  // CROSSPOINT_KOREADER_AUTOSYNC

  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      buttonEvents.drain();
      stopAutomaticPageTurn();
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  bool buttonPrevTurn = false;
  bool buttonNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm) {
      if (ev.type == ButtonEventManager::PressType::Long && KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::COMPARE);
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (pageHasPlaceholders) {
          forceLoadLargeImages = true;
          pageHasPlaceholders = false;
          requestUpdate();
          return;
        }
        openReaderMenu();
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (footnoteDepth > 0) {
          restoreSavedPosition();
          return;
        }
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        finish();
        return;
      }
    }

    // Built-in default for a long-press on a page-turn button is chapter skip
    // (prev for Left/PageBack, next for Right/PageForward). Non-default long
    // actions are dispatched by the global handler in main.cpp and never reach
    // here, so a Long event arriving for these buttons with the setting at
    // BTN_DEFAULT is the built-in case. The FSM emits no Short for a press that
    // already produced a Long, so the release cannot also turn the page.
    if (ev.type == ButtonEventManager::PressType::Long) {
      const bool prevChapter =
          (ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnLongPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnLongLeft == BA::BTN_DEFAULT);
      const bool nextChapter =
          (ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnLongPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnLongRight == BA::BTN_DEFAULT);
      if (prevChapter || nextChapter) {
        onButtonAction(nextChapter ? BA::BTN_NEXT_SECTION : BA::BTN_PREV_SECTION);
        return;
      }
    }

    // Page turns for all four navigation buttons come from the event queue, so a burst of
    // presses during a slow slice (AA pass, pre-render, section build) is replayed press by
    // press instead of collapsing into one. A non-default short action never arrives here —
    // main.cpp dispatches it globally — but the setting is re-checked so a future caller
    // that pushes events directly cannot turn a remapped button into a page turn.
    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT)) {
        buttonPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT)) {
        buttonNextTurn = true;
        continue;
      }
    }
  }

  // A tap on a footnote marker or cross-reference follows the link. Tested before the page-turn
  // zones and the menu tap: a marker can sit anywhere on the page, including inside one of them,
  // and it is the more specific target.
  if (handleLinkTouch()) {
    return;
  }

  // A centre-third tap, or the top-edge menu swipe, opens the reader menu.
  // Kept next to the page-turn zones so both touch paths live in one place; the
  // zones do not overlap (outer thirds vs centre), so the order is not
  // load-bearing. Inert unless the board has touch and the user left touch
  // reading controls on.
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openReaderMenu();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectTiltPageTurn();
  // Touch page turns join tilt here rather than in the button event queue: both
  // are gesture sources the queue does not carry, and both must lose to an
  // explicit button press. Inert on non-touch boards and when the user has set
  // touchReaderControls to Off.
  const ReaderUtils::TouchPageTurn touchTurn = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  prevTriggered = prevTriggered || touchTurn.prev;
  nextTriggered = nextTriggered || touchTurn.next;
  if (!prevTriggered && !nextTriggered) {
    if (!buttonPrevTurn && !buttonNextTurn) {
      // Input-idle and no page turn pending: hand the idle slice to the background
      // routines (deferred AA, then Background A pre-render, then Background B).
      serviceBackgroundWork();
      return;
    }
    prevTriggered = buttonPrevTurn;
    nextTriggered = buttonNextTurn;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      const int lastSpineIndex = epub->getSpineItemsCount() - 1;
      int lastPageIndex = 0;
      int lastPageCount = 0;
      {
        RenderLock lock(*this);
        if (section && currentSpineIndex == lastSpineIndex) {
          lastPageCount = section->pageCount;
          lastPageIndex = std::max(0, section->pageCount - 1);
        }
      }
      writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);
      finishedBookSyncSpineIndex_ = lastSpineIndex;
      finishedBookSyncPage_ = lastPageIndex;
      finishedBookSyncPageCount_ = lastPageCount;

      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, epub->getPath(), epub->getSeries(),
                                           epub->getSeriesIndex(), epub->getAuthor(), nullptr, nullptr,
                                           &EpubReaderActivity::onFinishedBookSyncRequested, this);
    } else {
      {
        RenderLock lock(*this);
        currentSpineIndex = epub->getSpineItemsCount() - 1;
        navTarget = NavigationTarget::makeLastPage();
      }
      requestUpdate();
    }
    return;
  }

  // No current section, attempt to rerender the book
  {
    RenderLock lock(*this);
    if (!section) {
      requestUpdate();
      return;
    }
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

void EpubReaderActivity::serviceBackgroundWork() {
  // Priority order, highest first. Each routine self-gates on its own pending state and
  // is expected to do a bounded amount of work and return so the next loop tick can
  // service input. Background A is already scheduled inside render() via pendingPreRender
  // and runs as the PreRender pass when requestUpdate() fires; from the idle loop we run
  // the deferred AA pass first, then Background B (next-section pre-build) — B also
  // self-gates on A having finished, since A drives perceived page-turn speed.
  runDeferredGrayscalePass();
  if (RenderLock::peek()) {
    return;
  }
  {
    RenderLock lock;
    if (pendingGrayscale_.active) {
      return;  // AA still owed (display bus busy); it keeps priority over B/C
    }
  }
  // Background C (build of the section the reader is waiting on) takes priority over A and B:
  // the user has nothing to read until it produces pages. A/B are look-ahead work for a section
  // that is already displayed, so they only matter once the current section is built.
  bool currentSectionBuildActive = false;
  {
    RenderLock lock;
    currentSectionBuildActive = section && section->hasActiveBuild();
  }
  if (currentSectionBuildActive) {
    stepCurrentSectionBuild();
    return;
  }
  stepBackgroundSectionBuild();
}

void EpubReaderActivity::serviceBackgroundDebugLog() {
#if DEBUG_BACKGROUND_WORK
  const unsigned long now = millis();
  if (lastBgDebugLogMs_ != 0UL && (now - lastBgDebugLogMs_) < 5000UL) {
    return;
  }
  lastBgDebugLogMs_ = now;
  RenderLock lock;
  // B state + gate inputs so a stalled B explains itself from the log alone (e.g. parked
  // in waitheap because free/contig sit below the BG_BUILD_* floors, or css=1 with too
  // little heap for Section::heapAllowsEmbeddedStyle()).
  static constexpr const char* kBStateNames[] = {"probe", "waitheap", "building", "settled"};
  LOG_INF("ERS",
          "BG work: A runs=%lu completes=%lu | B runs=%lu completes=%lu | C runs=%lu completes=%lu state=%s spine=%d "
          "css=%d borrow=%d preempt=%u | preReady=%d buildPct=%d free=%lu contig=%lu",
          static_cast<unsigned long>(bgCounters_.aRuns), static_cast<unsigned long>(bgCounters_.aCompletes),
          static_cast<unsigned long>(bgCounters_.bRuns), static_cast<unsigned long>(bgCounters_.bCompletes),
          static_cast<unsigned long>(bgCounters_.cRuns), static_cast<unsigned long>(bgCounters_.cCompletes),
          kBStateNames[static_cast<uint8_t>(backgroundBuildState_)], backgroundBuildSpineIndex_,
          lastRenderStats.embeddedStyle ? 1 : 0, backgroundBorrowActive_ ? 1 : 0,
          static_cast<unsigned>(backgroundPreemptCount_),
          (preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex) ? 1 : 0, backgroundBuildPercent_,
          static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  checkHeapIntegrity("idle_5s");
#endif
}

void EpubReaderActivity::runDeferredGrayscalePass() {
  // Guard: do not start the AA pass while the render task is still inside
  // completeDisplay() — both paths touch the SPI display bus and must not
  // run concurrently. isRefreshPending() is true from triggerDisplay() until
  // completeDisplay() clears it; when it returns false the render task has
  // finished all post-waveform SPI work and it is safe to issue new SPI writes.
  if (RenderLock::peek()) {
    return;
  }
  // Serialize both the guard reads and deferred AA with the render task. Reading
  // the shared_ptr itself before taking this lock is a C++ data race even when
  // the same conditions are checked again afterwards.
  RenderLock lock;
  if (!pendingGrayscale_.active || !pendingGrayscale_.page || renderer.isRefreshPending()) {
    return;
  }
  if (dropStagedWorkIfPositionMoved("deferred AA")) {
    return;  // the page this AA belongs to is no longer the one being read
  }
  // Full clock for the pass, as for the Background-B/C build slices. Usually redundant — the AA
  // pass is normally owed within IDLE_DOWNCLOCK_MS of the page turn that armed it, so the idle
  // saver has not downclocked yet (device trace 2026-08-07: AA ran ~1.7 s after the button press,
  // at 160 MHz). It matters in the case this pass is built around: AA self-gates on
  // !isRefreshPending(), and on X3 that stays asserted for the whole multi-second waveform, so a
  // HALF or full refresh can defer AA past the 3 s threshold and into low-power mode.
  // Taken after the early returns above so the idle ticks (the overwhelmingly common case) never
  // toggle the clock, and safe to take here because the render lock is already held, which the
  // render task's own per-render power lock also requires. The waveform waits this pass triggers
  // still downclock normally: they run on this same task, so enterWaveformWait() sees its own
  // lock owner rather than a foreign one.
  HalPowerManager::Lock powerLock;
  // The AA pass is the largest window in the reader with no heap sampling in it: 200-450 ms of
  // plane rendering plus a grayscale replay, all on the loop task. It is the prime suspect for
  // the unattributed Min Free watermark, so bracket it.
  logReaderMemSnapshot("aa_begin");
  pendingGrayscale_.active = false;
  renderer.setFastGrayscaleLut(pendingGrayscale_.fastLut);
  const int fontId = pendingGrayscale_.fontId;
  const int marginLeft = pendingGrayscale_.marginLeft;
  const int contentTop = pendingGrayscale_.contentTop;
  const Page* pagePtr = pendingGrayscale_.page.get();
  // This pass runs ON the loop task, so while it is in flight no input is sampled or dispatched
  // at all (measured: 1237 ms max loop duration on X3; a measurement at full clock on X3
  // 2026-08-07 showed ~530 ms — planes 133 + gray 230 + restore 49). The plane render therefore
  // aborts per element, and a bailed plane forces the pass to abort regardless of what the
  // predicate says — never let a partially drawn plane reach the panel.
  bool planeAborted = false;
  const auto gt = renderer.renderGrayscalePlanesSequential(
      [&](GfxRenderer::RenderMode) {
        if (!pagePtr->renderTextOnly(renderer, fontId, marginLeft, contentTop, /*abortable=*/true)) {
          planeAborted = true;
          return;
        }
        pagePtr->renderImagesFromGrayscaleCache(renderer, marginLeft, contentTop);
      },
      [&] { return planeAborted || aaPreemptedByNavigation(); });
  logReaderMemSnapshot("aa_after_planes");
  pendingGrayscale_.page.reset();
  logReaderMemSnapshot("aa_end");
  LOG_DBG("ERS", "Deferred AA%s: planes=%lums gray=%lums restore=%lums", gt.aborted ? " ABORTED" : "", gt.planesMs,
          gt.displayMs, gt.restoreMs);
  checkHeapIntegrity("after_deferred_aa");
  // The AA cleanup just reseeded frameBuffer from frameBufferActive (the current page),
  // so the buffer state is now correct for a pre-render. On X3, render() holds off the
  // PreRender pass while a deferred AA is owed (see the guard there); kick it now so a
  // pre-render armed during that window actually runs against the freshly-settled buffer.
  // A panel that runs AA inline never holds off the pre-render, so this re-request
  // would just be a redundant trigger there — skip it to keep that refresh sequence
  // unchanged.
  if (usesDeferredAa() && pendingPreRender) {
    requestUpdate();
  }
}

Section::BuildParams EpubReaderActivity::makeSectionBuildParams() const {
  const RenderLayout layout = computeRenderLayout();
  Section::BuildParams p;
  p.fontId = getEffectiveReaderFontId();
  p.lineCompression = getEffectiveReaderLineCompression();
  p.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  p.paragraphAlignment = getEffectiveParagraphAlignment();
  p.viewportWidth = layout.viewportWidth;
  p.viewportHeight = layout.viewportHeight;
  p.hyphenationEnabled = getEffectiveHyphenation();
  p.fontSizeNormalization = getEffectiveFontSizeNormalization();
  p.embeddedStyle = lastRenderStats.embeddedStyle;
  p.bionicReadingEnabled = getEffectiveBionicReading();
  // The SETTING, nothing else. Availability is guaranteed by construction: a preview-enabled
  // build resolves the note text its own spine needs before it lays out a single line (see
  // Section::resolveInlineFootnotePreviews), so this key can never describe a page cache that
  // says "previews on" while showing bare markers. It used to be ANDed with "is the book-level
  // cache ready", which meant the key flipped mid-session and dragged a rebuild behind it.
  p.inlineFootnotePreviews = getEffectiveInlineFootnotePreviews();
  p.imageRendering = lastRenderStats.imageRendering;
  p.fontSizeLadder = buildReaderFontSizeLadder(p.fontId);
  if (epub) {
    buildBookFontFamilies(
        bookFontFamilies_, epub->getCachePath(), p.fontId,
        bookFontSizeOverride >= 0 ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontPointSize);
    p.fontFamilies = &bookFontFamilies_;
  }
  return p;
}

void EpubReaderActivity::startActivityForResult(std::unique_ptr<Activity>&& activity,
                                                ActivityResultHandler resultHandler) {
  // See the header: B's borrow must not outlive the reader being the top activity, or every
  // redraw the child makes runs a HALF waveform. No-op unless B currently holds the block.
  //
  // endBackgroundBorrow() rather than resetBackgroundBuild(): the former discards only a build
  // that is still live, and keeps a COMPLETED one for buildSection() to adopt. Resetting would
  // throw that finished section away and make the next page turn rebuild it — paying for the
  // refresh fix with a page-turn stall.
  //
  // Under the render lock, like every other endBackgroundBorrow() call site. This runs on the
  // loop task, and the render task calls the same function from recoverSecondaryBufferIfNeeded()
  // at the top of every render(). Its `if (!backgroundBorrowActive_) return;` is a plain bool,
  // not a guard against concurrency: unlocked, both tasks pass it and both run the teardown —
  // double-destroying backgroundSection_ and buildScratch_ (two frees into TLSF, which corrupts
  // the free list and hangs a later malloc) while returnSecondaryBuffer() memcpy's 48 KB into a
  // framebuffer the render task is using.
  {
    RenderLock lock(*this);
    if (backgroundBorrowActive_) {
      LOG_INF("ERS", "Overlay '%s' opening; returning Background-B's borrowed buffer so its refreshes stay fast",
              activity ? activity->getName().c_str() : "<null>");
    }
    // Not a preemption. backgroundPreemptCount_ measures one specific thing — a build that keeps
    // losing the race against page turns — and BG_BUILD_MAX_PREEMPTIONS (2) makes B abandon the
    // spine to the foreground once it is hit. An overlay opening says nothing about whether the
    // parse fits between two turns, so letting it burn that budget would mean two visits to the
    // reader menu permanently demote the next section to a blocking Background-C build with its
    // popup. Restore the count so B resumes with exactly the budget it had.
    const uint8_t preemptionsBeforeOverlay = backgroundPreemptCount_;
    endBackgroundBorrow();
    backgroundPreemptCount_ = preemptionsBeforeOverlay;
    // The child draws over the page, so nothing staged for this screen survives it: the owed
    // deferred AA of the page on it, and a queued or finished pre-render of the next page (it lives
    // in the write framebuffer, which the child draws into). Left armed, the render that follows
    // the child's return was classified as that pre-render and drew nothing -- the menu stayed on
    // screen until the next page turn (X3 run 23: a lane redraw finished 11 ms before the menu
    // press, arming the pre-render, which the heap floor then skipped). Dropped, that render is a
    // Normal pass that redraws the page and stages its AA afresh.
    if (pendingGrayscale_.active || pendingPreRender || usePreRenderedBuffer || preRenderedPage.ready) {
      LOG_DBG("ERS", "Overlay '%s' opening; dropping the%s%s%s staged for the page under it",
              activity ? activity->getName().c_str() : "<null>", pendingGrayscale_.active ? " deferred AA" : "",
              pendingPreRender ? " pre-render request" : "", preRenderedPage.ready ? " pre-rendered page" : "");
    }
    pendingGrayscale_ = {};
    pendingPreRender = false;
    usePreRenderedBuffer = false;
    preRenderedPage = {};
    preRenderedPlanesStaged_ = false;
  }
  Activity::startActivityForResult(std::move(activity), std::move(resultHandler));
}

void EpubReaderActivity::suspendBackgroundWork() {
  {
    // Under the render lock, like every other borrow teardown: the render task
    // calls endBackgroundBorrow() from the top of every render(), and two
    // unlocked teardowns double-free the same Section.
    RenderLock lock(*this);
    if (backgroundWorkSuspended_) return;
    backgroundWorkSuspended_ = true;
    // endBackgroundBorrow(), NOT resetBackgroundBuild(). This releases what is
    // actually large — the lent 48 KB region and the build arena inside it —
    // and aborts only a build that is still live, which is resumable by design:
    // abortSectionBuild() keeps a completed extraction, so the retry skips the
    // inflate. A build that already FINISHED is left alone. Its Section is a
    // few hundred bytes of page-offset LUT, and the work itself is not in RAM
    // at all: the cache file on the card is the durable artifact, which is why
    // the look-ahead's own Probe state discards the Section object the moment
    // loadSectionFile() finds the file. Dropping it here would free nothing
    // worth having and cost the next visit a needless re-open.
    const uint8_t preemptionsBeforeOverlay = backgroundPreemptCount_;
    endBackgroundBorrow();
    // Same reasoning as startActivityForResult's: an overlay opening says
    // nothing about whether the parse fits between two page turns, so it must
    // not burn the budget that makes B abandon a spine.
    backgroundPreemptCount_ = preemptionsBeforeOverlay;
    // Discard the pre-rendered next page: it lives in the WRITE framebuffer
    // (renderPageContentOnly draws there, not into the secondary), which is exactly
    // what the overlay is about to draw over.
    preRenderedPage = {};
  }
  LOG_INF("ERS", "Background work suspended for a dictionary interaction");
}

void EpubReaderActivity::resumeBackgroundWork() {
  {
    RenderLock lock(*this);
    if (!backgroundWorkSuspended_) return;
    backgroundWorkSuspended_ = false;
  }
  // Nothing to restart explicitly: the look-ahead re-probes from Probe on the
  // next loop() and the pre-render re-arms on the next render().
  LOG_INF("ERS", "Background work resumed");
}

void EpubReaderActivity::resetBackgroundBuild() {
  endBackgroundBorrow();       // returns the lent buffer (and aborts the build) if B held it
  backgroundSection_.reset();  // ~Section aborts a partial build and deletes its partial file
  backgroundBuildSpineIndex_ = -1;
  backgroundBuildNeedsResolve_ = false;
  backgroundBuildGateCheckMs_ = 0;
  backgroundBuildState_ = BackgroundBuildState::Probe;
  backgroundBuildPercent_ = -1;
  backgroundPreemptCount_ = 0;
  backgroundBuildStarved_ = false;
}

bool EpubReaderActivity::beginBackgroundBorrow() {
  if (backgroundBorrowActive_ || secondaryBorrowed_ || !backgroundSection_) {
    return false;  // already lent (to B or to C) — never two arenas over one region
  }
  size_t borrowedSize = 0;
  uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize);
  if (!borrowed) {
    return false;  // no secondary buffer to lend (X3 baseline, or already released for a C build)
  }
  buildScratch_ = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
  if (!buildScratch_ || !buildScratch_->valid()) {
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    return false;
  }
  backgroundSection_->setExternalBuildScratch(buildScratch_.get());
  secondaryBorrowed_ = true;
  backgroundBorrowActive_ = true;
  // Mark AA unavailable while the block is away (renderContents gates on this as well as on
  // hasSecondaryBuffer). Unlike the Background-C borrow this deliberately does NOT seed RED RAM
  // or opt in to single-buffer fast differential:
  //  - No refresh can happen during B's borrow. Every render calls recoverSecondaryBufferIfNeeded()
  //    first, which hands the block back before the pass draws — so the single-buffer display
  //    path would never be exercised, and if one ever did slip through, an un-opted-in FAST
  //    downgrades to HALF (slow, correct) instead of ghosting against a bogus baseline.
  //  - syncRedRamFromFrameBuffer() would be actively wrong here. C borrows right after drawing
  //    its popup, so frameBuffer is what's on screen. B borrows during quiet reading, when a
  //    completed Background-A pre-render usually leaves the NEXT page in frameBuffer — seeding
  //    RED RAM from that makes the next page turn diff against the frame it is about to draw
  //    and leaves the page visibly unchanged.
  secondaryBufferDegraded_ = true;
  // What this build will really cost: the heap's low point while it holds the buffer, read back
  // in endBackgroundBorrow(). The allocator tracks it from here (it is a global setting, so the
  // [MEM] ticks' "Min Free" shows the same local low until the buffer goes back).
  backgroundBorrowFreeAtStart_ = static_cast<uint32_t>(esp_get_free_heap_size());
  backgroundHeapLowTracked_ = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  LOG_INF("ERS", "Background-B: borrowed secondary buffer for spine %d build (%u bytes, free=%lu contig=%lu)",
          backgroundBuildSpineIndex_, static_cast<uint32_t>(borrowedSize),
          static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  return true;
}

void EpubReaderActivity::endBackgroundBorrow() {
  if (!backgroundBorrowActive_) {
    return;
  }
  // Order matters: the build allocates INSIDE the region being handed back, so it must be torn
  // down first. ~Section aborts the in-flight build (releasing into the arena) and deletes the
  // partial cache file; the book-keyed inflated-HTML cache survives when phase (a) completed,
  // so a later attempt skips re-inflation (see Section::abortSectionBuild).
  const bool buildWasLive = backgroundSection_ && backgroundSection_->hasActiveBuild();
  if (backgroundSection_) {
    backgroundSection_->setExternalBuildScratch(nullptr);
    // Only a LIVE build still points into the region. A finished one tore its state down in
    // stepSectionBuild (buildState_.reset() before Done/Failed), so the Section is arena-free
    // and worth keeping — buildSection() can still adopt it as a completed target.
    if (buildWasLive) {
      backgroundSection_.reset();
    }
  }
  buildScratch_.reset();
  renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  backgroundBorrowActive_ = false;
  backgroundBuildThrough_ = false;
  secondaryBorrowed_ = false;
  secondaryBufferDegraded_ = false;
  backgroundBuildPercent_ = -1;
  // A build that was still live has been discarded: count the preemption and re-probe the target
  // (phase (a) may have banked the inflated HTML, so the retry can be much cheaper). The counter
  // is deliberately NOT reset here — it is what stops this from looping forever on a spine whose
  // parse cannot fit between two page turns.
  if (buildWasLive) {
    backgroundPreemptCount_++;
    backgroundBuildState_ = BackgroundBuildState::Probe;
    backgroundBuildGateCheckMs_ = 0;
  }
  uint32_t lowWhileLent = 0;
  if (backgroundHeapLowTracked_) {
    lowWhileLent = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    heap_caps_monitor_local_minimum_free_size_stop();
    backgroundHeapLowTracked_ = false;
  }
  LOG_INF("ERS",
          "Background-B: returned secondary buffer (spine %d, %s, preemptions=%u; heap %lu at start, %lu lowest)",
          backgroundBuildSpineIndex_, buildWasLive ? "build discarded" : "no build live",
          static_cast<unsigned>(backgroundPreemptCount_), static_cast<unsigned long>(backgroundBorrowFreeAtStart_),
          static_cast<unsigned long>(lowWhileLent));
  // A status refresh (clock minute, battery step) was held back while the buffer was lent; the
  // buffer is home, so draw it now. Not when a render is what took the buffer back: render()
  // clears the flag before it gets here, since it redraws the status bar anyway.
  if (statusRefreshDeferred_) {
    statusRefreshDeferred_ = false;
    requestUpdate();
  }
}

void EpubReaderActivity::stepBackgroundSectionBuild() {
  // B does SD I/O only, no SPI — but it must not contend with the render task for the
  // render lock while a waveform (or a render) is in flight: a blocked loop task cannot
  // service input. Skip the tick instead; idle ticks are plentiful while the user reads.
  // Note RenderLock::peek() returns true when the mutex is HELD (busy), not when free.
  if (RenderLock::peek()) {
    return;
  }
  // One lock for the whole step: every branch below touches the SD (even discarding a
  // stale build removes its partial file) and the parse slice reads glyph metrics from
  // the shared renderer, so all of it must be serialised against the render task.
  RenderLock lock;
  if (!epub || !section || readerPhase_ != ReaderPhase::READING || backgroundWorkSuspended_ || pendingPreRender ||
      usePreRenderedBuffer || renderer.isRefreshPending() || imageProcessingActive_) {
    return;
  }

  // B no longer has to wait for footnotes.bin. makeSectionBuildParams() now keys the variant on
  // actual preview availability, so a build started before the gather is cached under the
  // previews-OFF hash rather than masquerading as preview-enabled. Background A keeps priority:
  // pendingPreRender clears whether or not it produced a ready page.

  // Background A re-arm (one retry per displayed page): A's pass runs right after the
  // page render, while the deferred AA still holds the just-rendered page (~10 KB) —
  // its heap floor (PRE_RENDER_MIN_FREE_HEAP_BYTES) can refuse at that moment (first seen as
  // 55.9 KB free against what was then a 56 KB floor) and nothing retries it. Done HERE, under the render lock, because
  // it dereferences section state the render task mutates — an earlier unlocked version in serviceBackgroundWork()
  // raced buildSection's reassignment of `section`. B keeps waiting behind pendingPreRender until the retry has run,
  // preserving A's priority.
  //
  // Only for the page ON SCREEN. A navigation dispatched from main.cpp (double/long press, gesture)
  // moves the position and requests its render on this same tick, before the render task has run;
  // arming here then made that render a PreRender pass, which draws the page after the new one
  // off-screen and never flushes — the panel stayed on the old page (issue #351). The render that
  // draws the new page arms its own pre-render.
  const uint32_t preRenderFree = esp_get_free_heap_size();
  // Everything except the heap floor, so the floor can be reported on its own. Pre-render is a
  // nice-to-have (page-turn latency), not correctness — but a floor that rejects it constantly is
  // still evidence the floors are mistuned.
  const bool preRenderWanted =
      !preRenderedPage.ready && !backgroundBuildThrough_ && section->currentPage + 1 < section->pageCount &&
      lastRenderedSpineIndex_ == currentSpineIndex && lastRenderedPageIndex_ == section->currentPage &&
      (preRenderRearmSpine_ != currentSpineIndex || preRenderRearmPage_ != section->currentPage);
  if (preRenderWanted && preRenderFree < PRE_RENDER_MIN_FREE_HEAP_BYTES) {
    HEAP_GATE("preRenderArm", false, preRenderFree, PRE_RENDER_MIN_FREE_HEAP_BYTES, 0, 0);
  }
  if (preRenderWanted && preRenderFree >= PRE_RENDER_MIN_FREE_HEAP_BYTES) {
    preRenderRearmSpine_ = currentSpineIndex;
    preRenderRearmPage_ = section->currentPage;
    pendingPreRender = true;
    markStagedForCurrentPage();
    requestUpdate();
    return;
  }
  // Images the reader is about to reach outrank a section fifty pages away: warm them first,
  // then let B look ahead. Only while B is not holding the borrow -- a build in progress
  // finishes its slices; the next page turn hands the buffer back anyway.
  if (backgroundBuildState_ != BackgroundBuildState::Building && stepImageWarmLocked()) {
    return;
  }

  const int spineCount = epub->getSpineItemsCount();
  // Re-anchor the lookahead window whenever the reading position moves (any navigation):
  // the per-target state held below is then stale, and the cursor restarts at the new +1.
  if (backgroundBuildBaseSpine_ != currentSpineIndex) {
    resetBackgroundBuild();
    backgroundPausedForChapter_ = false;  // a new chapter: B may try again
    backgroundAbandonedSpine_ = -1;
    backgroundBuildBaseSpine_ = currentSpineIndex;
    backgroundBuildSpineIndex_ = currentSpineIndex + 1;
    backgroundWindowPagesBuilt_ = 0;
  }
  // The next chapter lost its build to page turns mid-chapter and was left to the foreground. On
  // the last pages a page turn no longer discards a build of it (BG_BUILD_THROUGH_PAGES), so it
  // gets one more try there. The window restarts with it: the chapters after it are cached and
  // settle for free, and their pages count against the budget again.
  if (backgroundAbandonedSpine_ == currentSpineIndex + 1 && backgroundBuildState_ != BackgroundBuildState::Building &&
      nearChapterEndForBuildThrough()) {
    LOG_INF("ERS", "Background-B: retrying spine %d on the last pages, after it was abandoned mid-chapter",
            backgroundAbandonedSpine_);
    resetBackgroundBuild();
    backgroundBuildSpineIndex_ = backgroundAbandonedSpine_;
    backgroundAbandonedSpine_ = -1;
    backgroundWindowPagesBuilt_ = 0;
  }
  // Walk forward from currentSpineIndex+1 to the book end. The cursor advances as each target
  // settles (Settled case below); already-cached spines settle for free in Probe.
  if (backgroundBuildSpineIndex_ < currentSpineIndex + 1 || backgroundBuildSpineIndex_ >= spineCount) {
    return;
  }
  // Page-budget gate: stop pre-building once ~BG_BUILD_LOOKAHEAD_PAGES of runway is laid out ahead
  // of the reader — the current section's unread tail plus the subsequent sections built so far.
  // Only gate at a section boundary (state==Probe, no build in flight) so a section in progress is
  // never abandoned mid-build; the runway shrinks as the reader advances, re-opening the window.
  if (backgroundBuildState_ == BackgroundBuildState::Probe) {
    // The last background build ran short of memory and was skipped: wait for the reader to
    // enter the next chapter before trying another (see backgroundPausedForChapter_). Gated here,
    // at a section boundary with no build in flight, for the same reason as the page budget
    // below: a section already in progress must never be abandoned holding the borrowed buffer.
    if (backgroundPausedForChapter_) {
      return;
    }
    const int currentTailPages = (section && section->pageCount > 0)
                                     ? std::max(0, static_cast<int>(section->pageCount) - 1 - section->currentPage)
                                     : 0;
    if (currentTailPages + backgroundWindowPagesBuilt_ >= BG_BUILD_LOOKAHEAD_PAGES) {
      return;
    }
  }
  const int targetSpine = backgroundBuildSpineIndex_;

  switch (backgroundBuildState_) {
    case BackgroundBuildState::Settled: {
      // Target indexed (freshly built or already cached): advance the cursor to the next
      // section. The window/navigation guard above stops the walk at the window or book end.
      const int next = targetSpine + 1;
      resetBackgroundBuild();
      backgroundBuildSpineIndex_ = next;
      return;  // re-probe `next` on the following tick
    }

    case BackgroundBuildState::Probe: {
      // One SD probe per target: if the exact cache variant already exists there is
      // nothing to pre-build.
      backgroundSection_ = std::make_unique<Section>(epub, targetSpine, renderer);
      const Section::BuildParams p = makeSectionBuildParams();
      const bool cached = backgroundSection_->loadSectionFile(p);
      if (cached && !backgroundSection_->isEmbeddedStyleFallback()) {
        backgroundWindowPagesBuilt_ += backgroundSection_->pageCount;  // already-built runway counts toward the budget
        backgroundSection_.reset();
        backgroundBuildState_ = BackgroundBuildState::Settled;
      } else {
        // Does this spine still owe the footnote resolver? Its build then runs one extra pass
        // before the layout parse — a SAX scan of the extracted XHTML, plus a stream of any note
        // document it points at that is not banked yet — which FootnotePreviews::Resolver spreads
        // across slices like any other build work.
        //
        // B used to refuse such a spine outright and leave it to the foreground, back when that
        // pass ran to completion inside one slice. The refusal was self-perpetuating: the
        // resolved bit is only ever set BY a build, and the only spine the foreground ever builds
        // is the one the reader just entered, so every subsequent chapter stayed unresolved, B
        // refused all of them, and look-ahead was dead for the whole book (issue #211, regression
        // in 2.24). The flag no longer gates the build — it only buys the pass the heap it holds
        // across those slices, and a resolve that fails anyway is discarded below, not cached.
        backgroundBuildNeedsResolve_ =
            getEffectiveInlineFootnotePreviews() && !FootnotePreviews::spineResolved(epub->getCachePath(), targetSpine);
        backgroundBuildState_ = BackgroundBuildState::WaitHeap;
      }
      return;  // one bounded step per tick
    }

    case BackgroundBuildState::WaitHeap: {
      // Re-check at most ~1×/s: the gates walk the heap free-list, and their outcome
      // only changes when other allocations move — not per ~70 ms loop tick.
      const unsigned long now = millis();
      if (backgroundBuildGateCheckMs_ != 0 && now - backgroundBuildGateCheckMs_ < 1000UL) {
        return;
      }
      backgroundBuildGateCheckMs_ = now;
      // Give-up latch: this target has already burned its retry budget being preempted. Leave it
      // to Background-C (the reader gets the popup, exactly as before this feature) and move the
      // cursor on, rather than re-losing the same race for the rest of the chapter.
      if (backgroundPreemptCount_ >= BG_BUILD_MAX_PREEMPTIONS) {
        LOG_INF("ERS", "Background build spine=%d abandoned after %u preemptions; leaving it to foreground",
                targetSpine, static_cast<unsigned>(backgroundPreemptCount_));
        if (targetSpine == currentSpineIndex + 1 && !nearChapterEndForBuildThrough()) {
          backgroundAbandonedSpine_ = targetSpine;  // retried on the last pages, see above
        }
        backgroundSection_.reset();
        backgroundBuildState_ = BackgroundBuildState::Settled;
        return;
      }
      // Preferred path: build inside the BORROWED secondary framebuffer. This is what makes B
      // viable at all on the reading heap — the arena absorbs the parse working set, the inflate
      // ring and the CSS index, so the heap-backed floors below (which the ~57 KB/~24 KB reading
      // steady state cannot reach) stop being the binding constraint. Only once the reader has
      // settled on a page: the borrow costs this page's AA if a turn lands during it.
      // beginBackgroundBorrow() self-declines when there is no secondary buffer to lend (X3
      // baseline, or a Background-C build already holds it), so this needs no device split.
      // Settle check. Measured from the last page ON SCREEN, not the last page TURN:
      // lastPageTurnTime is only stamped by turns, so before the reader's first turn it still
      // holds 0 and the quiet period is vacuously satisfied — B would take the buffer moments
      // after a book's first page appeared, which is precisely when the next turn is coming.
      // max() of the two so an explicit turn still resets the window even if no render followed.
      const unsigned long lastActivityMs = std::max(lastPageTurnTime, lastPageOnScreenMs_);
      // And do not start when a button edge is already queued: the borrow would be handed back
      // on the very next loop tick, wasting the slice AND burning one of the two attempts
      // BG_BUILD_MAX_PREEMPTIONS allows before B abandons the spine. This is the same predicate
      // the image decoders bail on, so "the user is waiting" means the same thing everywhere.
      // Safe to gate B on: nothing waits on B. (Background-C must NEVER be gated this way — the
      // foreground draws mid-build pages out of C's own progress, so blocking C on a pending
      // foreground draw deadlocks: the page is never built, so the draw stays pending forever.)
      const bool inputQueued = CooperativeAbort::shouldAbortLongTask();
      // A build that still owes the footnote resolve allocates that pass out of the heap even
      // when everything else it does lives in the borrowed arena, so it needs the margin on top.
      const uint32_t borrowFreeFloor =
          BG_BUILD_BORROW_MIN_FREE_HEAP_BYTES + (backgroundBuildNeedsResolve_ ? BG_BUILD_RESOLVE_EXTRA_HEAP_BYTES : 0);
      // The next chapter, a few pages away, does not wait for a quiet window: page turns no longer
      // discard its build (see BG_BUILD_THROUGH_PAGES).
      const bool buildThrough = targetSpine == currentSpineIndex + 1 && nearChapterEndForBuildThrough();
      if (!inputQueued && (buildThrough || (now - lastActivityMs) >= BG_BUILD_BORROW_QUIET_MS) &&
          esp_get_free_heap_size() >= borrowFreeFloor &&
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT) >=
              BG_BUILD_BORROW_MIN_CONTIG_HEAP_BYTES - LARGEST_FREE_BLOCK_SLACK &&
          beginBackgroundBorrow()) {
        backgroundBuildThrough_ = buildThrough;
        if (buildThrough) {
          LOG_INF("ERS", "Background-B: building spine %d through page turns (page %d of %u)", targetSpine,
                  section->currentPage + 1, section->pageCount);
        }
        backgroundBuildState_ = BackgroundBuildState::Building;
        backgroundBuildStarved_ = false;
        return;
      }
      // Settled and nothing queued, so only the heap or the buffer said no: see hasPendingBackgroundWork().
      backgroundBuildStarved_ = !inputQueued && (buildThrough || (now - lastActivityMs) >= BG_BUILD_BORROW_QUIET_MS);
      // No buffer to lend (a C build holds it, or a realloc never came back), or the reader is
      // not settled: wait. There is no heap-backed B any more (see the BG_BUILD_BORROW_* note).
      return;
    }

    case BackgroundBuildState::Building: {
      // Layout needs glyph metrics only (advance/kerning), never bitmaps — but the
      // reading-path prewarm leaves SD-font styles wired to a page-scoped FULL (bitmap)
      // cache covering only the displayed page's glyphs, and ensureFontReady's mmap
      // metadata fast-path no-ops when any cache is already wired. The build's layout
      // then misses on nearly every glyph and pulls BITMAPS through the 8-slot overflow
      // loader (~180 ms each; measured 7.4 s of a 9.6 s background build). Reset
      // accumulation so the parser's next metadata-only prewarm re-wires the
      // flash-resident full metric tables — the same thing the foreground indexing path
      // does before createSectionFile. Re-done every slice because an interleaved page
      // render re-enters page mode; for mmap fonts the re-wire is pointer assignments,
      // and the next foreground render rebuilds its page cache in its normal prewarm.
      renderer.clearFontAccumulation();
#if DEBUG_BACKGROUND_WORK
      bgCounters_.bRuns++;
#endif
      Section::BuildStep step;
      {
        // Run the slice at the normal clock. B is idle-time work by definition, so main.cpp's
        // idle power saver has already dropped the CPU to LOW_POWER_FREQ (10 MHz) by the time it
        // gets here — and BG_BUILD_BUDGET_MS is wall-clock, checked between ~1 KB visitor chunks.
        // At 10 MHz a single chunk's layout can run well past the whole budget on its own: measured
        // on device, 40 ms slices became 1220 ms loop stalls and a 57-page section took 28 s.
        // Holding the clock for the slice restores the intended granularity, and race-to-idle also
        // costs LESS energy than grinding at 10 MHz for 10x the wall time. Scoped to the slice
        // rather than the whole build so the render task's own per-render power lock (and the
        // waveform-wait downclock that depends on no foreign lock being held) still work normally;
        // safe because B only reaches here holding the render lock, which that path also needs.
        // Deliberately NOT taken for the Probe/WaitHeap ticks, which idle for minutes at a time
        // and would otherwise toggle the CPU clock on every loop iteration for no work.
        HalPowerManager::Lock powerLock;
        step = backgroundSection_->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
      }
      checkHeapIntegrity("after_b_slice");
      if (step == Section::BuildStep::More) {
        // Heap can drop after the WaitHeap gate passed (an interleaved page render allocates).
        // The moment the CSS resolver starts skipping lookups the result is doomed to be
        // css-degraded and discarded — bail now instead of grinding through the rest of the
        // build. Background-C will rebuild it released (clean) when the reader navigates in.
        if (backgroundSection_->activeBuildCssDegraded()) {
          LOG_INF("ERS", "Background build spine=%d css-degrading mid-build; aborting early for foreground rebuild",
                  targetSpine);
          backgroundSection_->abortSectionBuild();
          backgroundSection_.reset();
          endBackgroundBorrow();  // no build left to discard; just hands the block back
          backgroundBuildPercent_ = -1;
          backgroundBuildState_ = BackgroundBuildState::Settled;
          return;
        }
        backgroundBuildPercent_ = static_cast<int8_t>(backgroundSection_->activeBuildPercent());
        return;
      }

      backgroundBuildPercent_ = -1;
      if (step == Section::BuildStep::Done) {
        if (backgroundSection_->isTruncatedCache() || backgroundSection_->isCssLowHeapDegraded() ||
            backgroundSection_->isFootnotePreviewsUnresolved() || backgroundSection_->isImageHeaderDegraded() ||
            backgroundSection_->isTableRowDegraded()) {
          // Memory ran short mid-parse: pages are missing (truncated), CSS lookups were skipped
          // (styles silently absent from the cached pages), the footnote resolve could not
          // complete (markers left plain in a cache keyed "previews on"), or an image was dropped
          // to alt text because its header read was refused. Don't hand any of them to the
          // foreground: its blocking path runs with the secondary buffer released (~52 KB more
          // headroom) and will likely build it clean.
          //
          // B is the caller most exposed to the last two: it builds with the framebuffer
          // borrowed, which is exactly when the largest free block is small.
          const char* reason = backgroundSection_->isTruncatedCache()               ? "truncated"
                               : backgroundSection_->isCssLowHeapDegraded()         ? "css-degraded"
                               : backgroundSection_->isFootnotePreviewsUnresolved() ? "footnotes unresolved"
                               : backgroundSection_->isImageHeaderDegraded()        ? "image degraded"
                                                                                    : "table row demoted";
          LOG_INF("ERS",
                  "Background build spine=%d %s; discarding for foreground rebuild, look-ahead paused until the "
                  "next chapter",
                  targetSpine, reason);
          backgroundSection_->clearCache();
          backgroundSection_.reset();
          backgroundPausedForChapter_ = true;
        } else {
#if DEBUG_BACKGROUND_WORK
          bgCounters_.bCompletes++;
#endif
          backgroundWindowPagesBuilt_ += backgroundSection_->pageCount;  // count this section toward the page budget
          LOG_INF("ERS", "Background build spine=%d complete: %u pages", targetSpine, backgroundSection_->pageCount);
          SerialTestApi::noteBuildDone(targetSpine, backgroundSection_->pageCount);
          // The build recorded its page count (SpinePageIndex). If that firms up the chapter
          // counter on screen, redraw it now rather than at the next page turn:
          // endBackgroundBorrow() below requests the deferred status refresh.
          chapterSpanSpine_ = -1;
          if (section && chapterCounterShown()) {
            const ChapterPageSpan::Display chapter = chapterPageDisplay();
            if (chapter.total != lastStatusBarChapterTotal_ || chapter.approximate != lastStatusBarChapterApprox_) {
              statusRefreshDeferred_ = true;
            }
          }
          // The image lane's window may reach into this section: a window marked clean before
          // its cache existed has to be looked at again.
          imageWarmCleanSpine_ = -1;
        }
      } else {
        LOG_ERR("ERS", "Background build spine=%d failed; look-ahead paused until the next chapter", targetSpine);
        backgroundSection_.reset();
        backgroundPausedForChapter_ = true;
      }
      // Terminal for this target: the build state is torn down either way, so give the borrowed
      // framebuffer back at once rather than holding it across the Settled tick — AA is off for
      // every render until it returns. A completed backgroundSection_ survives this (it no longer
      // references the arena) so buildSection() can still adopt it.
      // Resolve the image headers this build deferred BEFORE the borrowed framebuffer goes back
      // (#249): device-measured on the X3, reader-time contiguous heap tops out around 31.7 KB
      // while the walk's ring needs 33.3 KB — at every moment of a session, this one included.
      // The borrowed region is the only block of that size, and the build that used it as an
      // arena is finished (its state is torn down before Done/Failed; a completed Section no
      // longer references it), so reset() reclaims the build's bump allocations and the walk
      // carves its ring there. Also flushes whatever this build resolved (valid regardless of the
      // build's outcome). One write per completed background section, under the render lock.
      if (buildScratch_) buildScratch_->reset();
      epub->persistImageManifest(buildScratch_.get());
      endBackgroundBorrow();
      backgroundBuildState_ = BackgroundBuildState::Settled;
      return;
    }
  }
}

bool EpubReaderActivity::stepImageWarmLocked() {
  if (!section || section->hasActiveBuild() || section->pageCount == 0) return false;
  if (secondaryBorrowed_ || backgroundBorrowActive_ || !renderer.hasSecondaryBuffer()) return false;
  const int cur = section->currentPage;
  if (imageWarmCleanSpine_ == currentSpineIndex && imageWarmCleanPage_ == cur) return false;
  // When to start. A decode holds the loop for seconds and yields to input only by aborting, so:
  //  - no gesture in flight: the abort hook fires on NEW edges, and a tap whose edges were
  //    already drained (released, waiting out the 300 ms double-click window) would be held for
  //    the whole decode -- runs 17/18 caught three page turns firing 18 ms after a lane decode;
  //  - a short settle after a turn, a draw or a preemption, so a reader flipping pages is not
  //    handed a decode to abort on every page. The deferred AA pass needs no allowance of its
  //    own: the scheduler runs nothing while it is owed, and it ends ~0.35 s after the draw.
  // It used to wait B's 1.5 s borrow settle. Run 18, at 6-7 s per page: a 570 KB progressive
  // (~4.5 s) lost to the page turn four times, once within ~100 ms of finishing; the second
  // that settle sat on was the difference. An abort costs the turn a few ms (the decoders poll
  // per band and every few KB of the index pass), so starting early is cheap.
  const unsigned long now = millis();
  const unsigned long lastActivityMs = std::max({lastPageTurnTime, lastPageOnScreenMs_, imageWarmPreemptedMs_});
  if (CooperativeAbort::shouldAbortLongTask() || buttonEvents.isGestureInFlight() ||
      now - lastActivityMs < IMAGE_LANE_SETTLE_MS) {
    return false;
  }

  // Large images are warmed too (forceLoad), whatever the placeholder setting says: that setting
  // exists because a decode on a page turn is slow, and this is the decode nobody waits for.
  // Once the cache exists the image renders directly (wouldShowPlaceholder is false for a cached
  // image). Run 17: the chapter's opening illustration was left as a placeholder and cost 6.3 s
  // when the reader asked for it.
  const bool warmGrayscale = getEffectiveTextAntiAliasing() && !secondaryBufferDegraded_;

  // The window, in reading order: the page on screen first (a placeholder there is what the
  // reader is looking at), then the next kImageWarmLookahead pages of this section. Loading a
  // page is an SD read of a few KB, so the scan checks for input between pages.
  const int lastPage = static_cast<int>(section->pageCount) - 1;
  const int windowEnd = cur + kImageWarmLookahead;
  for (int p = cur; p <= std::min(lastPage, windowEnd); ++p) {
    // A page the reader's own ladder just gave up on is not climbed again here: every step was refused a moment ago.
    if (imageWarmGaveUp(currentSpineIndex, p) || memoryDegradedPage(currentSpineIndex, p)) continue;
    if (CooperativeAbort::shouldAbortLongTask()) return false;
    section->currentPage = p;
    auto page = section->loadPageFromSectionFile();
    section->currentPage = cur;
    if (!page || !page->hasUncachedImages(/*forceLoad=*/true, /*monochromeOutput=*/true, warmGrayscale)) continue;
    return warmPageForImageLane(*page, currentSpineIndex, p, /*onScreen=*/p == cur, warmGrayscale);
  }

  // The window runs past this section's end: continue into the next section's first pages when
  // their cache exists (Background-B built it, or an earlier session did). A chapter often opens
  // on an illustration, and without this the turn onto it always decoded in the foreground. A
  // no-CSS fallback cache is skipped: the reader rebuilds that one on entry.
  const int spill = windowEnd - lastPage;
  const int nextSpine = currentSpineIndex + 1;
  if (spill > 0 && nextSpine < epub->getSpineItemsCount()) {
    auto next = makeUniqueNoThrow<Section>(epub, nextSpine, renderer);
    if (next && next->loadSectionFile(makeSectionBuildParams()) && !next->isEmbeddedStyleFallback()) {
      const int nextLast = std::min<int>(static_cast<int>(next->pageCount), spill) - 1;
      for (int p = 0; p <= nextLast; ++p) {
        if (imageWarmGaveUp(nextSpine, p)) continue;
        if (CooperativeAbort::shouldAbortLongTask()) return false;
        next->currentPage = p;
        auto page = next->loadPageFromSectionFile();
        if (!page || !page->hasUncachedImages(true, true, warmGrayscale)) continue;
        return warmPageForImageLane(*page, nextSpine, p, /*onScreen=*/false, warmGrayscale);
      }
    }
  }
  // Nothing to warm in the window: remember that until the position changes (or B completes a
  // section the window reaches into -- see the Background-B completion).
  imageWarmCleanSpine_ = currentSpineIndex;
  imageWarmCleanPage_ = cur;
  return false;
}

bool EpubReaderActivity::warmPageForImageLane(const Page& page, const int spine, const int pageIndex,
                                              const bool onScreen, const bool warmGrayscale) {
  // What the page on screen shows is the 1-bit variant; if that was already cached (only the
  // Bayer companion is missing) the screen is right and needs no redraw.
  const bool shownWasUncached = onScreen && page.hasUncachedImages(true, /*monochromeOutput=*/true, false);

  // The lent secondary buffer is the decoders' scratch (work pool, bands, progressive
  // workspace), exactly as the page-turn warm borrows it; the framebuffer is not written at
  // all (ScopedCacheOnlyImageWrites), so the page on screen -- or a pre-rendered next page --
  // is untouched and no clearScreen follows. One page per tick.
  // At the offsets the page render draws the images at, not (0, 0): the ordered dither baked into a
  // .pxc follows screen position, so a lane-warmed cache used to differ in phase from the one a
  // page render writes -- and a decode the lane parks (see RenderConfig::checkpointPath) resumes
  // only for the very same configuration, so a page turn onto the image could never pick it up.
  const RenderLayout layout = computeRenderLayout();
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - layout.marginTop - layout.marginBottom);
  const int contentTop = layout.marginTop + getImageOnlyPageYOffset(page, viewportHeight);

  size_t borrowedSize = 0;
  uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize);
  if (!borrowed) return false;
  auto scratch = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
  const unsigned long t0 = millis();
  imageProcessingActive_ = true;
  CooperativeAbort::clearAborted();
  BuildArena* const arena = scratch && scratch->valid() ? scratch.get() : nullptr;
  const uint8_t accents = sdFontSystem.bookAccentsLoaded();
  ImageMemoryLadder::Action ladder;
  {
    image_scratch::ScopedArena scratchScope(arena);
    GfxRenderer::ScopedCacheOnlyImageWrites cacheOnly(renderer);
    // Cheaper here than on a page turn: a page whose images the lane decodes is drawn from their caches.
    ladder = warmImagesClimbingLadder(page, layout.marginLeft, contentTop, /*forceLoad=*/true, warmGrayscale, arena,
                                      /*drawsPage=*/false, accents);
  }
  // Preempted by input: not a failure, the next quiet moment retries.
  const bool preempted = CooperativeAbort::consumeAborted();
  imageProcessingActive_ = false;
  scratch.reset();
  renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  if (finishLadder(ladder, accents)) return true;

  const bool complete = !page.hasUncachedImages(true, true, warmGrayscale);
  if (!complete && !preempted) noteImageWarmMiss(spine, pageIndex);
  if (preempted) imageWarmPreemptedMs_ = millis();
  // The lazy load: the page on screen showed these images as placeholders, and their caches now
  // exist, so a plain redraw renders them (from the cache, no decode). Not while input waits --
  // the press it belongs to is about to replace this page anyway.
  const bool shownNowCached = shownWasUncached && !page.hasUncachedImages(true, true, false);
  const bool inputWaiting = CooperativeAbort::shouldAbortLongTask();
  const bool redraw = shownNowCached && !inputWaiting;
  // The page after the one on screen just became pre-renderable. Its pre-render ran when this
  // page was drawn and skipped it for its uncached images (as did the one re-arm, if it has run
  // since), so nothing would try again before the turn. Keyed on the page actually on screen,
  // not on currentPage: a navigation dispatched from main.cpp moves the position before its
  // render runs, and arming then would shelve that render as a PreRender pass (#351).
  const bool nextPageNowPreRenderable =
      complete && !inputWaiting && spine == currentSpineIndex && lastRenderedSpineIndex_ == currentSpineIndex &&
      lastRenderedPageIndex_ == section->currentPage && pageIndex == section->currentPage + 1 &&
      !preRenderedPage.ready && !pendingPreRender && !backgroundBuildThrough_;
  if (nextPageNowPreRenderable) {
    pendingPreRender = true;
    markStagedForCurrentPage();
  }
  const char* note = !complete                  ? (preempted ? " -- preempted by input, retries after the next settle"
                                                   : imageWarmGaveUp(spine, pageIndex) ? " -- incomplete, giving up on this page"
                                                                                       : " -- incomplete, will retry once")
                     : redraw                   ? " -- on screen, redrawing"
                     : shownNowCached           ? " -- on screen, input waiting (the next render shows it)"
                     : nextPageNowPreRenderable ? " -- next page, pre-rendering it"
                                                : "";
  LOG_INF("ERS", "Image lane: spine %d page %d warmed in %lums%s (free=%lu contig=%lu)", spine, pageIndex,
          millis() - t0, note, static_cast<unsigned long>(esp_get_free_heap_size()),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  checkHeapIntegrity("after_image_lane");
  if (redraw || nextPageNowPreRenderable) requestUpdate();
  return true;
}

bool EpubReaderActivity::imageWarmGaveUp(const int spine, const int page) const {
  return std::any_of(imageWarmMisses_.begin(), imageWarmMisses_.end(), [&](const ImageWarmMiss& m) {
    return m.spine == spine && m.page == page && m.tries >= kImageWarmMaxTries;
  });
}

namespace {
const char* ladderActionName(const ImageMemoryLadder::Action action) {
  switch (action) {
    case ImageMemoryLadder::Action::Done:
      return "done";
    case ImageMemoryLadder::Action::LightRelief:
      return "light relief";
    case ImageMemoryLadder::Action::HeavyRelief:
      return "heavy relief";
    case ImageMemoryLadder::Action::DropAccent:
      return "an accent family fewer";
    case ImageMemoryLadder::Action::Emergency:
      return "emergency: images as boxes";
    case ImageMemoryLadder::Action::Restart:
      return "restart";
  }
  return "?";
}
}  // namespace

void EpubReaderActivity::prewarmPageFonts(const Page& page, const int xOffset, const int yOffset) {
  auto scope = renderer.getFontCacheManager()->createPrewarmScope();
  page.renderTextOnly(renderer, getEffectiveReaderFontId(), xOffset, yOffset);  // scan pass
  scope.endScanAndPrewarm();
}

ImageMemoryLadder::Action EpubReaderActivity::warmImagesClimbingLadder(const Page& page, const int xOffset,
                                                                       const int yOffset, const bool forceLoad,
                                                                       const bool warmGrayscale, BuildArena* arena,
                                                                       const bool drawsPage, const uint8_t accents) {
  using ImageMemoryLadder::Action;
  using ImageMemoryLadder::Decode;
  using ImageMemoryLadder::Step;
  ImageMemoryLadder::Input in;
  in.accents = accents;
  for (;;) {
    in.ttfLoaded = sdFontSystem.ttfLoaded();
    // A live build has laid text out with the set, and goes on from where it is.
    in.canUnload =
        !(section && section->hasActiveBuild()) && !(backgroundSection_ && backgroundSection_->hasActiveBuild());
    in.fontsBack = true;
    const uint32_t heapShortBefore = image_scratch::heapShortCount();
    if (arena) arena->reset();  // nothing of the step before lives in it
    switch (in.step) {
      case Step::Plain:
        break;
      case Step::LightRelief: {
        // The page's glyphs into the store first: drawn from there, the page needs no miss afterwards.
        if (drawsPage) prewarmPageFonts(page, xOffset, yOffset);
        renderer.releaseFontCaches();
        const uint32_t freed = sdFontSystem.releaseTtfMissMemory();
        LOG_INF("ERS", "Memory ladder: light relief, %lu B of TrueType memory freed (free=%lu)",
                static_cast<unsigned long>(freed), static_cast<unsigned long>(esp_get_free_heap_size()));
        break;
      }
      case Step::HeavyRelief:
        renderer.releaseFontCaches();
        sdFontSystem.suspendTtf(renderer);
        LOG_INF("ERS", "Memory ladder: heavy relief, the TrueType set unloaded (free=%lu)",
                static_cast<unsigned long>(esp_get_free_heap_size()));
        break;
    }
    page.warmImageCaches(renderer, xOffset, yOffset, forceLoad, /*monochromeOutput=*/true, warmGrayscale);
    // What the step freed comes back at once, while the decode's buffers are just freed and the heap least broken up.
    switch (in.step) {
      case Step::Plain:
        break;
      case Step::LightRelief:
        in.fontsBack = sdFontSystem.restoreTtfMissMemory();
        break;
      case Step::HeavyRelief: {
        const unsigned long start = millis();
        in.fontsBack = sdFontSystem.resumeTtf(renderer);
        LOG_INF("ERS", "Memory ladder: the TrueType set %s in %lu ms (free=%lu)",
                in.fontsBack ? "loaded again" : "did not load again", millis() - start,
                static_cast<unsigned long>(esp_get_free_heap_size()));
        break;
      }
    }
    in.decode = CooperativeAbort::wasAborted()                            ? Decode::Failed
                : !page.hasUncachedImages(forceLoad, true, warmGrayscale) ? Decode::Done
                : image_scratch::heapShortCount() != heapShortBefore      ? Decode::HeapShort
                                                                          : Decode::Failed;
    const Action action = ImageMemoryLadder::next(in);
    switch (action) {
      case Action::LightRelief:
        in.step = Step::LightRelief;
        continue;
      case Action::HeavyRelief:
        in.step = Step::HeavyRelief;
        continue;
      case Action::Done:
      case Action::DropAccent:
      case Action::Emergency:
      case Action::Restart:
        if (in.step == Step::Plain && in.decode == Decode::Done) {
          LOG_DBG("ERS", "Memory ladder: decoded without relief (free=%lu)",
                  static_cast<unsigned long>(esp_get_free_heap_size()));
        } else {
          LOG_INF("ERS", "Memory ladder: %s after step %u (free=%lu)", ladderActionName(action),
                  static_cast<unsigned>(in.step), static_cast<unsigned long>(esp_get_free_heap_size()));
        }
        return action;
    }
  }
}

bool EpubReaderActivity::finishLadder(const ImageMemoryLadder::Action action, const uint8_t accents) {
  switch (action) {
    case ImageMemoryLadder::Action::Done:
    case ImageMemoryLadder::Action::LightRelief:
    case ImageMemoryLadder::Action::HeavyRelief:
    case ImageMemoryLadder::Action::Emergency:
      return false;
    case ImageMemoryLadder::Action::DropAccent:
      sdFontSystem.limitBookAccents(static_cast<uint8_t>(accents - 1));
      break;
    case ImageMemoryLadder::Action::Restart:
      // The images are cached by now; a fresh heap is what the fonts need.
      saveStateForRestart();
      if (trySilentRestartToReaderForHeapRecovery()) return true;
      // Refused: one heap restart since the last cold boot already. The page is drawn with its images as boxes
      // then, without the ladder, or the next render would climb it to here again.
      if (section) {
        memoryDegradedSpine_ = currentSpineIndex;
        memoryDegradedPage_ = section->currentPage;
      }
      break;
  }
  // The book's fonts as it has them now, with an install for a set the store does not hold, and the chapter laid out
  // with them at the next render. A set is never left without its miss memory: a layout would measure the glyphs it
  // cannot draw as missing.
  if (sdFontSystem.ttfMissMemoryReleased() && !sdFontSystem.restoreTtfMissMemory()) unloadSdFontIfLoaded();
  ensureSdFontLoadedForPath(epub->getPath().c_str());
  preRenderedPage = {};  // a font popup may have drawn over it
  fontRelayoutPending_ = true;
  requestUpdate();
  return true;
}

void EpubReaderActivity::noteImageWarmMiss(const int spine, const int page) {
  for (auto& m : imageWarmMisses_) {
    if (m.spine == spine && m.page == page) {
      if (m.tries < kImageWarmMaxTries) m.tries++;
      return;
    }
  }
  auto& slot = imageWarmMisses_[imageWarmMissNext_];
  imageWarmMissNext_ = static_cast<uint8_t>((imageWarmMissNext_ + 1) % imageWarmMisses_.size());
  slot.spine = static_cast<int16_t>(spine);
  slot.page = static_cast<int16_t>(page);
  slot.tries = 1;
}

void EpubReaderActivity::stepCurrentSectionBuild() {
  // Don't contend with the render task for the lock while a refresh/render is in flight: a
  // blocked loop task can't service input. peek() is true when the mutex is HELD (busy).
  if (RenderLock::peek()) {
    return;
  }
  RenderLock lock;
  if (!epub || !section || !section->hasActiveBuild() || renderer.isRefreshPending() || imageProcessingActive_) {
    return;
  }

  // Discard the in-flight build and re-render with more headroom. Two retry flavours, chosen by
  // the caller via `retryIncremental`:
  //  - true  (low-heap abort of a RESIDENT build): retry as IncrementalReleased — the release
  //    frees the ~52 KB the build was starved of, and the build stays sliced so the first page
  //    still appears mid-build. Latched via forceReleasedBuildSpine_.
  //  - false (parse failure / truncated / css-degraded): retry on the old blocking released path
  //    (latched via forceBlockingBuildSpine_; compileSectionCache honours the latch by forcing
  //    the buffer release).
  const auto fallbackToReleasedRebuild = [&](const char* reason, const bool retryIncremental) {
    LOG_ERR("ERS", "Background-C spine=%d %s; falling back to released %s rebuild", currentSpineIndex, reason,
            retryIncremental ? "incremental" : "blocking");
    // Why did the BORROWED build fail — was the arena out of room, or the heap? The comment below
    // asserts the latter ("CSS-heavy books need the full freed block"), and the whole released
    // path rests on it, but the claim has never been separated from its alternative. It is also
    // circular as stated: a released build has no arena, so CSS resolves from the HEAP, and that
    // is what needs ~90 KB — a borrowed build resolves out of the arena at the lean floor.
    //
    // BuildArena exposes exactly the pair that decides it (see BuildArena.h: failedAllocSize is
    // the last REFUSED size, highWater only records successes):
    //   failedAlloc != 0                     -> the arena really ran out; the release is earned.
    //   failedAlloc == 0, highWater << cap   -> the arena was never the constraint; the failure is
    //                                           heap-side and the release treats the wrong cause.
    // Logged before the teardown below destroys the arena. See
    // docs/memory-allocation-strategy.md §9.2.
    if (buildScratch_) {
      LOG_ERR("ERS", "Background-C spine=%d arena: highWater=%u/%u failedAlloc=%u releaseFails=%lu free=%lu contig=%lu",
              currentSpineIndex, static_cast<uint32_t>(buildScratch_->highWater()),
              static_cast<uint32_t>(buildScratch_->capacity()), static_cast<uint32_t>(buildScratch_->failedAllocSize()),
              static_cast<unsigned long>(buildScratch_->releaseFailures()),
              static_cast<unsigned long>(esp_get_free_heap_size()),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
    } else {
      LOG_ERR("ERS", "Background-C spine=%d arena: none (heap-backed build) free=%lu contig=%lu", currentSpineIndex,
              static_cast<unsigned long>(esp_get_free_heap_size()),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
    }
    section->clearCache();
    section.reset();  // aborts the in-flight build, releasing into the scratch arena
    // If this build ran inside the BORROWED secondary buffer, hand it back before the released
    // rebuild: borrow only gives phase-b reading-heap (~62 KB), so CSS-heavy books that need the
    // full freed block (~90 KB) fall back here to the legacy release path, which needs the buffer
    // back on the heap first. The return cannot fail (the region never entered the heap). Leave a
    // clean flag state; compileSectionCache re-establishes release/degraded/fast-diff as needed.
    if (secondaryBorrowed_) {
      buildScratch_.reset();
      renderer.returnSecondaryBuffer();
      secondaryBorrowed_ = false;
      secondaryBufferDegraded_ = false;
      renderer.setSingleBufferFastDiff(false);
      LOG_INF("ERS", "Background-C spine=%d: returned borrowed secondary buffer before released rebuild",
              currentSpineIndex);
    }
    if (retryIncremental) {
      forceReleasedBuildSpine_ = currentSpineIndex;
    } else {
      forceBlockingBuildSpine_ = currentSpineIndex;
    }
    readerPhase_ = ReaderPhase::READING;
    buildingPopupShown_ = false;
    buildDisplayedPage_ = -1;
    backgroundBuildPercent_ = -1;
    requestUpdate();  // -> BuildSection -> released (incremental or blocking) build
  };

  // Layout needs glyph metrics only; reset accumulation so the next prewarm re-wires the
  // flash-resident metric tables rather than the page-scoped bitmap cache (see
  // stepBackgroundSectionBuild for the full rationale).
  renderer.clearFontAccumulation();
#if DEBUG_BACKGROUND_WORK
  bgCounters_.cRuns++;
#endif
  Section::BuildStep step;
  {
    // Full clock for the slice, same reasoning as the Background-B slice: this runs on the loop
    // task, and a build long enough to matter has already passed IDLE_DOWNCLOCK_MS with no
    // button press, so main.cpp's idle saver has dropped the CPU to LOW_POWER_FREQ. The wall-clock
    // BG_BUILD_BUDGET_MS is only checked between ~1 KB visitor chunks, so at 10 MHz one chunk
    // overruns the whole budget and the slice stops being a slice. Worse here than for B: this is
    // the build the reader is actually waiting on behind the "Indexing" popup.
    HalPowerManager::Lock powerLock;
    step = section->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
  }
  checkHeapIntegrity("after_c_slice");

  if (step == Section::BuildStep::More) {
    // Proactive low-heap guard: while the build is resident (AA buffer kept), bail to the released
    // path before heap reaches the fault zone. Only meaningful when the buffer is still resident —
    // a build already running released has that headroom and should ride it out.
    const uint32_t residentFree = esp_get_free_heap_size();
    const uint32_t residentContig = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
    constexpr uint32_t residentAbortContigFloor = RESIDENT_BUILD_ABORT_CONTIG_HEAP_BYTES - LARGEST_FREE_BLOCK_SLACK;
    const bool residentAbort = !secondaryBufferDegraded_ && (residentFree < RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES ||
                                                             residentContig < residentAbortContigFloor);
    if (residentAbort) {
      HEAP_GATE("residentAbort", false, residentFree, RESIDENT_BUILD_ABORT_FREE_HEAP_BYTES, residentContig,
                residentAbortContigFloor);
      fallbackToReleasedRebuild("low heap mid-build", /*retryIncremental=*/true);
      return;
    }
    backgroundBuildPercent_ = static_cast<int8_t>(section->activeBuildPercent());
    // A chapter-list jump (TocIndex) or a link into this spine (Anchor) used to resolve only when
    // the build completed, so the reader sat behind the popup for the whole build even when its
    // page was the first one laid out (device run 14: 7.6 s for "Chapter Three"). Ask the live
    // build where the target lands; once known it becomes a page target, which the draw below
    // and the completion path both understand -- and the answer is the one the finished cache
    // would give.
    if (navTarget.kind == NavigationTarget::Kind::TocIndex || navTarget.kind == NavigationTarget::Kind::Anchor) {
      const std::optional<uint16_t> landed = navTarget.kind == NavigationTarget::Kind::TocIndex
                                                 ? section->activeBuildPageForTocIndex(navTarget.tocIndex)
                                                 : section->activeBuildPageForAnchor(navTarget.anchorStr);
      if (landed) {
        LOG_INF("ERS", "Background-C spine=%d: target resolved mid-build to page %u", currentSpineIndex,
                static_cast<unsigned>(*landed));
        section->currentPage = *landed;
        settleFollowedLink(*landed);
        navTarget = NavigationTarget::makePage(*landed);
      }
    }
    // If the page the user is waiting on just became readable, ask the render task to draw it.
    const int want = section->currentPage;
    if (navTarget.kind == NavigationTarget::Kind::Page && want >= 0 &&
        want < static_cast<int>(section->activeBuildPageCount()) && want != buildDisplayedPage_) {
      requestUpdate();
    }
    return;
  }

  backgroundBuildPercent_ = -1;

  // Failed, or finished but truncated / CSS-degraded / with a table row demoted to paragraphs:
  // discard and retry on the released path. The latch (set inside the helper) stops buildSection
  // from re-entering Background-C for this spine.
  //
  // The demoted row counts here because a build that survives must escalate on purpose, or the
  // demoted layout is what gets cached. Arena builds lay a grid row's cell lines out in the lent
  // region (ChapterHtmlSlimParser::layoutTableRow), so a borrowed build now demotes a row only
  // when even the row's few KB of heap objects are refused (docs/memory-allocation-strategy.md
  // §8.4a). Before that, the X3 demoted every row of the Roosevelt appendix-b at 11-18 KB free
  // (2026-09-26); the released rebuild, with ~95 KB free, lays the table out.
  if (step == Section::BuildStep::Failed || section->isTruncatedCache() || section->isCssLowHeapDegraded() ||
      section->isTableRowDegraded()) {
    fallbackToReleasedRebuild(step == Section::BuildStep::Failed ? "failed"
                              : section->isTableRowDegraded()    ? "table row demoted"
                                                                 : "incomplete",
                              /*retryIncremental=*/false);
    return;
  }

  // Done, but possibly with an image dropped to alt text because its header walk was deferred
  // (SOF behind a large metadata block; the ring could not be had mid-parse — #249). The parser
  // and its arena are gone now, so resolve the deferred headers here; if that answered any, the
  // manifest serves the dimensions with no ring at all and a rebuild comes out clean. Nothing
  // resolved means a rebuild would do nothing different, so the build stands as it is.
  // Ring storage for the walk: the borrowed arena when C holds one (idle now — the build state is
  // gone), else the heap, which on the released path still contains the freed buffer because
  // recoverSecondaryBufferIfNeeded() only restores it on the next render.
  if (secondaryBorrowed_ && buildScratch_) buildScratch_->reset();
  const bool imageHeadersResolved = epub->persistImageManifest(secondaryBorrowed_ ? buildScratch_.get() : nullptr);
  if (section->isImageHeaderDegraded() && imageHeadersResolved) {
    fallbackToReleasedRebuild("image headers resolved after build", /*retryIncremental=*/true);
    return;
  }

  // Done & clean: the on-disk LUT is written and `section` is now a complete cache. Resolve the
  // navigation target now that the final page count is known, then transition to reading.
#if DEBUG_BACKGROUND_WORK
  bgCounters_.cCompletes++;
#endif
  LOG_INF("ERS", "Background-C spine=%d complete: %u pages", currentSpineIndex, section->pageCount);
  SerialTestApi::noteBuildDone(currentSpineIndex, section->pageCount);
  readerPhase_ = ReaderPhase::READING;

  // Resolve the display position. For a Page target, section->currentPage already tracked the
  // user's position through any mid-build page turns, so DON'T resolveInto (it would reset to the
  // original page). Clamp it; if they ran past the end while building, cross into the next spine.
  const int spineCount = epub->getSpineItemsCount();
  if (navTarget.kind == NavigationTarget::Kind::Page) {
    // A restored position whose chapter this build laid out anew lands on its paragraph, unless the
    // reader has already turned away from the page shown while it built.
    if (section->currentPage == navTarget.page) {
      if (const auto p = navTarget.relaidOutRestorePage(*section, currentSpineIndex)) {
        section->currentPage = *p;
      }
    }
    if (section->currentPage >= section->pageCount) {
      if (currentSpineIndex + 1 < spineCount) {
        navTarget = NavigationTarget::makePage(0);
        currentSpineIndex++;
        section.reset();
      } else if (currentSpineIndex + 1 == spineCount) {
        navTarget = NavigationTarget::makeLastPage();
        currentSpineIndex++;
        section.reset();
      } else {
        section->currentPage = std::max(0, section->pageCount - 1);
      }
    } else if (section->currentPage < 0) {
      section->currentPage = 0;
    }
  } else {
    navTarget.resolveInto(*section, currentSpineIndex);
    settleFollowedLink(section->currentPage);
  }
  anchorNavTargetToCurrentPage();
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  buildingPopupShown_ = false;
  buildDisplayedPage_ = -1;
  requestUpdate();  // -> Normal pass renders the resolved page (with AA)
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(const int percent) {
  int targetSpineIndex = 0;
  float spineProgress = 0.0f;
  if (!resolvePercent(percent, targetSpineIndex, spineProgress)) {
    return;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    navTarget = NavigationTarget::makePercent(spineProgress);
    section.reset();
  }
}

bool EpubReaderActivity::resolvePercent(int percent, int& targetSpineIndex, float& spineProgress) const {
  if (!epub) {
    return false;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return false;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return false;
  }

  targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  spineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (spineProgress < 0.0f)
    spineProgress = 0.0f;
  else if (spineProgress > 1.0f)
    spineProgress = 1.0f;
  return true;
}

void EpubReaderActivity::openDictionary() {
  {
    RenderLock lock(*this);
    if (!section) return;
  }

  if (SETTINGS.dictionaryName[0] == '\0') {
    // Send the user somewhere they can act rather than showing a popup that
    // only tells them what is missing.
    suspendBackgroundWork();
    startActivityForResult(std::make_unique<DictionarySelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             resumeBackgroundWork();
                             requestUpdate();
                           });
    return;
  }

  // During an active build the on-disk LUT is not written yet, so the page has
  // to come from the in-memory one -- the same choice the pre-render pass makes.
  std::unique_ptr<Page> page;
  {
    RenderLock lock(*this);
    if (!section) return;
    page = section->hasActiveBuild() ? section->loadPageFromActiveBuild(static_cast<uint16_t>(section->currentPage))
                                     : section->loadPageFromSectionFile();
  }
  if (!page) {
    LOG_ERR("ERS", "Dictionary: could not load the current page");
    return;
  }

  const RenderLayout layout = computeRenderLayout();
  const int effectiveFontId = getEffectiveReaderFontId();
  // Before startActivityForResult, which only hands back the borrowed buffer:
  // the overlay wants the heap a completed look-ahead section is sitting on.
  suspendBackgroundWork();
  startActivityForResult(
      std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page), effectiveFontId,
                                                     layout.marginLeft, layout.marginTop),
      [this](const ActivityResult&) {
        resumeBackgroundWork();
        requestUpdate();
      });
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      int spineIdx;
      int tocIdx;
      {
        RenderLock lock(*this);
        spineIdx = currentSpineIndex;
        tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                         : epub->getTocIndexForSpineIndex(currentSpineIndex);
      }
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx, tocIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            // The chapter list's own paint consumed the override armed before it launched
            // (one-shot, see consumeRefreshOverride); arm a fresh one here so the resumed
            // reader page gets a clean HALF_REFRESH instead of a FAST diff against RED RAM
            // that still holds the chapter list's last frame.
            ReaderUtils::enforceExitFullRefresh(renderer);
            RenderLock lock(*this);
            const auto& chapter = std::get<ChapterResult>(result.data);
            auto resolvedPage = (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                    ? section->getPageForTocIndex(*chapter.tocIndex)
                                    : std::nullopt;
            if (resolvedPage) {
              section->currentPage = *resolvedPage;
              anchorNavTargetToCurrentPage();
              forceLoadLargeImages = false;
              pageHasPlaceholders = false;
            } else {
              navTarget =
                  chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex) : NavigationTarget::makePage(0);
              currentSpineIndex = chapter.spineIndex;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY:
      openDictionary();
      break;
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      // Show each entry's note text when the book-level cache can supply it — see
      // footnotePreviewsForCurrentPage() for when opening the list may gather it.
      auto linkInfo = pageLinkInfoForCurrentPage();
      startActivityForResult(
          std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes,
                                                        std::move(linkInfo.previews), std::move(linkInfo.isNote)),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& footnoteResult = std::get<FootnoteResult>(result.data);
              navigateToHref(footnoteResult.href, true);
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      {
        RenderLock lock(*this);
        if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
          const float chapterProgress =
              static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
          bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
        }
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PRINTED_PAGE: {
      if (!epub) break;
      // Integer label range for the numeric input's bounds. Streamed (not loadPrintedPageList) so
      // the whole list is never held in RAM — see Epub::hasNumericPrintedPages for why.
      int minLabel = 0;
      int maxLabel = 0;
      if (!epub->getPrintedPageLabelRange(minLabel, maxLabel)) {
        break;  // no integer labels — shouldn't happen if the menu item was shown
      }

      // Pre-fill with the printed page the reader is currently on (or the nearest one before
      // it — rendered device pages rarely carry an anchor themselves, but they sit between
      // two printed pages, so the closest prior anchor is the "you're here" hint). Falls
      // back to the lowest integer label in the book if no prior anchor exists.
      int initialValue = minLabel;
      {
        RenderLock lock(*this);
        if (section) {
          if (const auto rawLabel =
                  section->getNearestPrintedPageLabelAtOrBefore(static_cast<uint16_t>(section->currentPage))) {
            if (const auto n = parsePrintedPageLabel(*rawLabel)) {
              initialValue = *n;
            }
          }
        }
      }

      startActivityForResult(
          std::make_unique<EpubReaderPrintedPageInputActivity>(renderer, mappedInput, initialValue, minLabel, maxLabel),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pick = std::get<PrintedPageResult>(result.data);
            // pick.label is always a numeric string (std::to_string of the picked value). Resolve it
            // back to a (href, anchor) by streaming the list again — no full-list vector retained.
            const auto value = parsePrintedPageLabel(pick.label);
            if (!value) return;
            const auto entry = epub->findPrintedPageByLabel(*value);
            if (!entry) {
              LOG_DBG("ERS", "printed-page jump: label '%s' not found in pagelist", pick.label.c_str());
              return;
            }
            const int spineIdx = epub->resolveHrefToSpineIndex(entry->href);
            if (spineIdx < 0) {
              LOG_DBG("ERS", "printed-page jump: could not resolve spine for href=%s", entry->href.c_str());
              return;
            }
            {
              RenderLock lock(*this);
              currentSpineIndex = spineIdx;
              navTarget =
                  entry->anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(entry->anchor);
              section.reset();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      std::unique_ptr<Page> p;
      {
        RenderLock lock(*this);
        if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
          p = section->loadPageFromSectionFile();
        }
      }
      if (p) {
        std::string fullText;
        for (const auto& el : p->elements) {
          if (el->getTag() == TAG_PageLine) {
            const auto& line = static_cast<const PageLine&>(*el);
            if (line.getBlock()) {
              const auto& block = *line.getBlock();
              const uint16_t wordCount = block.wordCount();
              for (uint16_t i = 0; i < wordCount; ++i) {
                if (!fullText.empty()) fullText += " ";
                fullText += block.wordText(i);
              }
            }
          }
        }
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STAR_PAGE: {
      {
        RenderLock lock(*this);
        if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
          bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
          requestUpdate();
        }
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STARRED_PAGES: {
      startActivityForResult(
          std::make_unique<StarredPagesActivity>(renderer, mappedInput, bookmarkStore, epub),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& starred = std::get<StarredPageResult>(result.data);
              RenderLock lock(*this);
              if (currentSpineIndex != starred.spineIndex || !section || section->currentPage != starred.pageNumber) {
                currentSpineIndex = starred.spineIndex;
                navTarget = NavigationTarget::makePage(starred.pageNumber);
                section.reset();
              }
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      if (tryAutoPushOnClose()) return;
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS_FOR_BOOK: {
      // Jump to this book's detail screen using the same filename-hash docId
      // the session was opened with. The in-flight session's time isn't
      // visible here — it lands in the store only when end() runs on reader
      // exit. For a brand-new book that's never been finished a session yet
      // the screen will show "no data"; that's accurate.
      if (!epub) break;
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(
                                 renderer, mappedInput, KOReaderDocumentId::calculateFromFilename(epub->getPath())),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_INFO: {
      if (!epub) break;
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, epub->getPath()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MARK_AS_READ: {
      if (!epub) {
        break;
      }
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount > 0) {
        const int lastSpineIndex = spineCount - 1;
        int lastPageIndex = 0;
        int lastPageCount = 0;
        {
          RenderLock lock(*this);
          if (section && currentSpineIndex == lastSpineIndex) {
            lastPageCount = section->pageCount;
            lastPageIndex = std::max(0, section->pageCount - 1);
          }
        }
        if (lastPageCount > 0) {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);
        } else {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
        }
        finishedBookSyncSpineIndex_ = lastSpineIndex;
        finishedBookSyncPage_ = lastPageCount > 0 ? lastPageIndex : 0;
        finishedBookSyncPageCount_ = lastPageCount;
      }
      BookFinished::launchFinishedBookFlow(*this, renderer, mappedInput, epub->getPath(), epub->getSeries(),
                                           epub->getSeriesIndex(), epub->getAuthor(), nullptr, nullptr,
                                           &EpubReaderActivity::onFinishedBookSyncRequested, this);
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache(true);
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
          if (!bookmarkStore.isEmpty()) {
            bookmarkStore.markDirty();
            bookmarkStore.save();
            GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                           false);
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::RENDER_BENCHMARK: {
#if ENABLE_BENCHMARKS
      runRenderBenchmark();
#endif  // ENABLE_BENCHMARKS
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PULL_REMOTE: {
      // One-tap pull path: run network preconditions and apply remote progress
      // directly instead of showing an intermediate chooser screen.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PULL_REMOTE);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PUSH_LOCAL: {
      // One-tap push path: run network preconditions and upload local progress
      // directly for KOReader-like "sync now" behavior.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PUSH_LOCAL);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::COMPARE_REMOTE: {
      // The menu's third sync entry: the same compare-then-choose flow a long Confirm press starts.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::COMPARE);
      }
      break;
    }
  }
}

void EpubReaderActivity::applyPendingBookmarkJump() {
  auto& jump = APP_STATE.pendingBookmarkJump;
  if (!jump.active || !epub || jump.bookPath != epub->getPath()) {
    return;
  }
  LOG_DBG("ERS", "Applying pending bookmark jump: spine=%u page=%u", jump.spineIndex, jump.pageNumber);
  if (jump.spineIndex >= static_cast<uint16_t>(epub->getSpineItemsCount())) {
    LOG_ERR("ERS", "Invalid bookmark jump spine index %u, resetting to 0", jump.spineIndex);
    jump.spineIndex = 0;
    jump.pageNumber = 0;
  }
  // Seed live state directly; the persistent write is for crash recovery only.
  // saveProgress() on the next render overwrites with the real percent.
  currentSpineIndex = jump.spineIndex;
  navTarget = NavigationTarget::makePage(jump.pageNumber);
  navTarget.cachedSpineIdx = jump.spineIndex;
  if (!writeReaderProgressCache(epub->getCachePath(), jump.spineIndex, jump.pageNumber, 0, 0)) {
    LOG_ERR("ERS", "Failed to persist bookmark jump to progress.bin; live state still seeded");
  }
  jump.clear();
  APP_STATE.saveToFile();
}

#if SERIAL_TEST_API
void EpubReaderActivity::applyPendingPercentJump() {
  auto& jump = APP_STATE.pendingPercentJump;
  if (!jump.active || !epub || jump.bookPath != epub->getPath()) {
    return;
  }
  int spine = 0;
  float progress = 0.0f;
  if (resolvePercent(jump.percent, spine, progress)) {
    LOG_DBG("ERS", "Applying pending percent jump: %d%% -> spine=%d at %.3f", jump.percent, spine, progress);
    currentSpineIndex = spine;
    navTarget = NavigationTarget::makePercent(progress);
  }
  jump = {};
}
#endif  // SERIAL_TEST_API

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::applyTextDarkness(const uint8_t textDarkness) {
  if (SETTINGS.textDarkness == textDarkness) {
    return;
  }
  SETTINGS.textDarkness = textDarkness;
  SETTINGS.saveToFile();
  renderer.setTextDarkness(textDarkness);
  // Force a re-render so the new darkness is visible immediately.
  requestUpdate();
}

void EpubReaderActivity::stopAutomaticPageTurn() {
  if (!automaticPageTurnActive) {
    return;
  }

  automaticPageTurnActive = false;

  if (UITheme::getStatusBarHeight(true) == UITheme::getStatusBarHeight()) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  RenderLock lock(*this);
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
    navTarget.cachedPageCount = section->pageCount;
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  preRenderedPage.ready = false;
  preRenderedPlanesStaged_ = false;
  pendingGrayscale_ = {};
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_LABELS)) {
    stopAutomaticPageTurn();
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  // Reset cached section when automatic page turn adds a forced status item band.
  if (UITheme::getStatusBarHeight(true) != UITheme::getStatusBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }
    section.reset();
  }
}

void EpubReaderActivity::applyBookReaderOverrides(const int8_t embeddedStyleOverride,
                                                  const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                                  const std::string& sdFontFamilyOverride,
                                                  const int8_t fontSizeOverride, const bool bionicReadingOverride,
                                                  const int8_t paragraphAlignmentOverride) {
  applyBookReaderOverrides(embeddedStyleOverride, imageRenderingOverride, fontFamilyOverride, sdFontFamilyOverride,
                           fontSizeOverride, static_cast<int8_t>(bionicReadingOverride ? 1 : 0),
                           paragraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
                           bookFontSizeNormalizationOverride, bookGuideDotsOverride,
                           bookInlineFootnotePreviewsOverride);
}

void EpubReaderActivity::applyBookReaderOverrides(
    const int8_t embeddedStyleOverride, const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
    const std::string& sdFontFamilyOverride, const int8_t fontSizeOverride, const int8_t bionicReadingOverride,
    const int8_t paragraphAlignmentOverride, const int8_t textAntiAliasingOverride, const int8_t hyphenationOverride,
    const int8_t fontSizeNormalizationOverride, const int8_t guideDotsOverride,
    const int8_t inlineFootnotePreviewsOverride) {
  if (!epub) {
    return;
  }

  // Built-in and SD font overrides are mutually exclusive; explicit built-in wins.
  int8_t normalizedFontFamilyOverride = fontFamilyOverride;
  std::string normalizedSdFontFamilyOverride = sdFontFamilyOverride;
  if (normalizedFontFamilyOverride >= 0) {
    normalizedSdFontFamilyOverride.clear();
  } else if (!normalizedSdFontFamilyOverride.empty()) {
    normalizedFontFamilyOverride = -1;
  }

  // Guide dots are excluded from this comparison on purpose: they are render-time
  // only (see TextBlock::setGuideDots), so a guide-dots-only change must not fall
  // through to the section relayout below.
  const bool layoutOverridesUnchanged =
      bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride &&
      bookFontFamilyOverride == normalizedFontFamilyOverride &&
      bookSdFontFamilyOverride == normalizedSdFontFamilyOverride && bookFontSizeOverride == fontSizeOverride &&
      bookBionicReadingOverride == bionicReadingOverride &&
      bookParagraphAlignmentOverride == paragraphAlignmentOverride &&
      bookTextAntiAliasingOverride == textAntiAliasingOverride && bookHyphenationOverride == hyphenationOverride &&
      bookFontSizeNormalizationOverride == fontSizeNormalizationOverride &&
      bookInlineFootnotePreviewsOverride == inlineFootnotePreviewsOverride;

  if (layoutOverridesUnchanged && bookGuideDotsOverride == guideDotsOverride) {
    return;
  }

  bookEmbeddedStyleOverride = embeddedStyleOverride;
  bookImageRenderingOverride = imageRenderingOverride;
  bookFontFamilyOverride = normalizedFontFamilyOverride;
  bookSdFontFamilyOverride = normalizedSdFontFamilyOverride;
  bookFontSizeOverride = fontSizeOverride;
  bookBionicReadingOverride = bionicReadingOverride;
  bookParagraphAlignmentOverride = paragraphAlignmentOverride;
  bookTextAntiAliasingOverride = textAntiAliasingOverride;
  bookHyphenationOverride = hyphenationOverride;
  bookFontSizeNormalizationOverride = fontSizeNormalizationOverride;
  bookGuideDotsOverride = guideDotsOverride;
  bookInlineFootnotePreviewsOverride = inlineFootnotePreviewsOverride;
  RECENT_BOOKS.setReaderOverrides(
      epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
      bookSdFontFamilyOverride, bookFontSizeOverride, bookBionicReadingOverride, bookParagraphAlignmentOverride,
      bookTextAntiAliasingOverride, bookHyphenationOverride, bookFontSizeNormalizationOverride, bookGuideDotsOverride,
      bookInlineFootnotePreviewsOverride);

  if (layoutOverridesUnchanged) {
    // Only guide dots changed: persisted above, and the repaint on resume picks
    // the new value up in render(). No section relayout, no refresh override.
    return;
  }

  // A changed override forces a full section relayout (section.reset() below → rebuild with the
  // "Indexing…" popup). That popup FAST-refreshes against whatever is on the panel; when the change
  // arrived via the full-screen selector, the extra menu/submenu/selector redraws leave the FAST
  // baseline out of sync and the popup box ghosts. Arm a one-shot HALF so drawPopup() establishes a
  // clean baseline — the same deliberate-transition signal the chapter/percent/footnote jumps use
  // (hasRefreshOverridePending() at the popup then also arms forceHalfRefreshAfterPopup_ for the
  // first content page). Reached only when something actually changed (early-out above), so routine
  // no-op reopens of the menu don't pay for it.
  ReaderUtils::enforceExitFullRefresh(renderer);

  RenderLock lock(*this);

  // The SD font was only ever (re)loaded on BOOK OPEN: ensureSdFontLoadedForPath() was called from
  // ReaderActivity::onEnter() and nowhere else. Changing the font or the size
  // from the reader menu therefore left the previously loaded face in place, and because
  // resolveFontId() demanded an exact point-size match, a size change fell back to the built-in
  // family until the book was closed and reopened. Both symptoms, one missing call. The resolver
  // now serves any size from the loaded face through a scaled alias, but that alias is made by
  // this very call (SdCardFontManager::ensureSizeAlias), so it is still the fix.
  //
  // Safe here and not earlier: the overrides are already persisted to RECENT_BOOKS above, which is
  // what the path-based resolution reads, and the RenderLock this holds is the one the cold-load
  // popup expects its caller to own.
  if (epub) ensureSdFontLoadedForPath(epub->getPath().c_str());
  resetSectionKeepingPosition();
}

void EpubReaderActivity::resetSectionKeepingPosition() {
  if (section) {
    const int currentPage = section->currentPage;
    if (!section->hasActiveBuild()) {
      if (const auto paragraphIndex = section->getParagraphIndexForPage(currentPage)) {
        navTarget = NavigationTarget::makeParagraph(*paragraphIndex, currentPage);
      } else {
        navTarget = NavigationTarget::makePage(currentPage);
      }
      navTarget.cachedPageCount = section->pageCount;
    } else {
      // pageCount is only the number of pages produced so far. Treating it as a
      // completed layout count would proportionally jump forward after relayout.
      navTarget = NavigationTarget::makePage(currentPage);
      LOG_DBG("ERS", "Preserving page %d without rescale during active section build", currentPage);
    }
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  section.reset();
}

bool EpubReaderActivity::getEffectiveEmbeddedStyle() const {
  if (bookEmbeddedStyleOverride >= 0) {
    return bookEmbeddedStyleOverride != 0;
  }
  return SETTINGS.embeddedStyle != 0;
}

bool EpubReaderActivity::getEffectiveBionicReading() const {
  if (bookBionicReadingOverride >= 0) {
    return bookBionicReadingOverride > 0;
  }
  return SETTINGS.bionicReading;
}

uint8_t EpubReaderActivity::getEffectiveImageRendering() const {
  if (bookImageRenderingOverride >= 0) {
    return static_cast<uint8_t>(bookImageRenderingOverride);
  }
  return SETTINGS.imageRendering;
}

bool EpubReaderActivity::getEffectiveTextAntiAliasing() const {
  if (bookTextAntiAliasingOverride >= 0) {
    return bookTextAntiAliasingOverride != 0;
  }
  return SETTINGS.textAntiAliasing != 0;
}

bool EpubReaderActivity::getEffectiveHyphenation() const {
  if (bookHyphenationOverride >= 0) {
    return bookHyphenationOverride != 0;
  }
  return SETTINGS.hyphenationEnabled != 0;
}

bool EpubReaderActivity::getEffectiveFontSizeNormalization() const {
  if (bookFontSizeNormalizationOverride >= 0) {
    return bookFontSizeNormalizationOverride != 0;
  }
  return SETTINGS.fontSizeNormalization != 0;
}

bool EpubReaderActivity::getEffectiveGuideDots() const {
  if (bookGuideDotsOverride >= 0) {
    return bookGuideDotsOverride != 0;
  }
  return SETTINGS.guideDots != 0;
}

bool EpubReaderActivity::getEffectiveInlineFootnotePreviews() const {
  if (bookInlineFootnotePreviewsOverride >= 0) {
    return bookInlineFootnotePreviewsOverride != 0;
  }
  return SETTINGS.inlineFootnotePreviews != 0;
}

uint8_t EpubReaderActivity::getEffectiveParagraphAlignment() const {
  if (bookParagraphAlignmentOverride >= 0) {
    return static_cast<uint8_t>(bookParagraphAlignmentOverride);
  }
  return SETTINGS.paragraphAlignment;
}

float EpubReaderActivity::getEffectiveReaderLineCompression() const {
  const uint8_t pointSize =
      (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontPointSize;
  const int effectiveFontId = getEffectiveReaderFontId();
  const int notosansId = CrossPointSettings::getBuiltinReaderFontIdForPoints(CrossPointSettings::NOTOSANS, pointSize);

  if (effectiveFontId == notosansId) {
    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.90f;
      case CrossPointSettings::NORMAL:
      default:
        return 0.95f;
      case CrossPointSettings::WIDE:
        return 1.0f;
    }
  }

  switch (SETTINGS.lineSpacing) {
    case CrossPointSettings::TIGHT:
      return 0.95f;
    case CrossPointSettings::NORMAL:
    default:
      return 1.0f;
    case CrossPointSettings::WIDE:
      return 1.1f;
  }
}

int EpubReaderActivity::getEffectiveReaderFontId() const { return resolveReaderFont(nullptr); }

int EpubReaderActivity::resolveReaderFont(std::string* familyOut) const {
  // Per-book font override: when set, force a specific BUILT-IN family even if
  // an SD card font is the global default. This makes the override predictable
  // ("override forces back to a known built-in") and avoids surprising users
  // who set the override before they had any SD fonts.
  const uint8_t pointSize =
      (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontPointSize;
  if (bookFontFamilyOverride >= 0) {
    if (familyOut) *familyOut = fontFamilyOptionLabel(static_cast<uint8_t>(bookFontFamilyOverride));
    return CrossPointSettings::getBuiltinReaderFontIdForPoints(static_cast<uint8_t>(bookFontFamilyOverride), pointSize);
  }
  // Then the book's SD family, then its embedded body family (Publisher Fonts, selected for it at book
  // open), then the global one, each only while it is loaded at this size (resolveSdCardFontId() is 0
  // for a family taken off the card); otherwise the built-in family. With no size override and no
  // book family this is SETTINGS.getReaderFontId().
  const char* publisherFamily =
      (bookSdFontFamilyOverride.empty() && epub) ? sdFontSystem.bookFamilyNameFor(epub->getCachePath()) : "";
  for (const char* sdFamily :
       {bookSdFontFamilyOverride.c_str(), publisherFamily, static_cast<const char*>(SETTINGS.sdFontFamilyName)}) {
    if (sdFamily[0] == '\0') continue;
    const int id = resolveSdCardFontId(sdFamily, pointSize);
    if (id != 0) {
      if (familyOut) *familyOut = sdFamily;
      return id;
    }
  }
  if (familyOut) *familyOut = fontFamilyOptionLabel(SETTINGS.fontFamily);
  return CrossPointSettings::getBuiltinReaderFontIdForPoints(SETTINGS.fontFamily, pointSize);
}

// Every size of a built-in family, as percents of a body at bodyPt.
static FontSizeLadder builtinFontSizeLadder(const uint8_t family, const uint8_t bodyPt) {
  FontSizeLadder ladder;
  for (const auto& rung : CrossPointSettings::FONT_SIZE_RUNGS) {
    ladder.addRung(CrossPointSettings::getBuiltinReaderFontId(family, rung.size),
                   static_cast<uint16_t>(rung.points * 100 / bodyPt));
  }
  return ladder;
}

// Sibling-size ladder for a built-in body font: every size of the same family, with its
// point size expressed as a percent of the body's. Deterministic from the fontId ALONE —
// the section-cache property hash deliberately excludes the ladder on that basis, so any
// path that rebuilds a section (foreground, background, sleep) derives an identical ladder
// from the same fontId. Unknown ids (SD-card fonts, one loaded size) get an empty ladder,
// which keeps the pure-scale fallback.
//
// Glyph-cache note: an earlier taller-heading-font attempt thrashed the FontDecompressor's
// four page slots. Two things changed since: FontCacheManager now prewarms per fontId, and
// the parser caps sections at ONE auxiliary font (body R/B/I + aux R = exactly four slots).
static FontSizeLadder buildReaderFontSizeLadder(const int bodyFontId) {
  // Sizes and their point values come from CrossPointSettings::FONT_SIZE_RUNGS, the one table
  // that defines the ladder; this used to keep its own pair of arrays and went stale.
  static constexpr uint8_t kFamilies[] = {CrossPointSettings::BOOKERLY, CrossPointSettings::NOTOSANS};
  const auto& rungs = CrossPointSettings::FONT_SIZE_RUNGS;
  constexpr int rungCount = CrossPointSettings::FONT_SIZE_RUNG_COUNT;
  // FontSizeLadder is fixed-capacity and drops extra rungs without a word, so the size that
  // would be lost is the one added last -- the largest, which is exactly the one a heading is
  // most likely to want. Caught here rather than at runtime.
  static_assert(rungCount <= FontSizeLadder::kMaxRungs,
                "FontSizeLadder::kMaxRungs is smaller than the reader ladder; raise it or headings "
                "will silently resample from a smaller face");

  FontSizeLadder ladder;
  // A TTF family draws every size with a face of its own.
  if (const auto native = sdFontSystem.ttfNativeSizes(bodyFontId)) {
    ladder.setNative(native->base, native->bodyPpem, native->minPx, native->maxPx);
    return ladder;
  }
  for (const uint8_t family : kFamilies) {
    for (int i = 0; i < rungCount; ++i) {
      if (CrossPointSettings::getBuiltinReaderFontId(family, rungs[i].size) != bodyFontId) continue;
      return builtinFontSizeLadder(family, rungs[i].points);
    }
  }
  return ladder;  // SD font or unknown id: empty ladder = scale-only fallback
}

// Publisher Fonts' Accents and All Text: the book's other families, for the blocks set in them. A generic serif or
// sans-serif goes to the built-in family of that kind, unless the body is already in it (the reader's own family, or
// the generic the book's body names: the reader's font stands for that). Monospace stays the reader's font: no
// monospace face is built in. Each of the book's accent families is in it when it is loaded beside the body: a
// built-in font, a TrueType reader font, or the book's own body family in All Text (not beside a .cpfont reader
// font); one the glyph store had no room for is not.
// Deterministic from the book's selection and the body font, as the ladder is from the body font.
static void buildBookFontFamilies(BlockFontFamilies& families, const std::string& cachePath, const int bodyFontId,
                                  const uint8_t pointSize) {
  families = {};
  const BookFonts::Families* book = sdFontSystem.bookFontFamiliesFor(cachePath);
  if (!book) return;
  families.body = book->body;
  // The built-in families in CssFontFamily's order: kSerif (1) is Bookerly, kSansSerif (2) Noto Sans.
  static_assert(CrossPointSettings::BOOKERLY + 1 == CssFontFamily::kSerif &&
                    CrossPointSettings::NOTOSANS + 1 == CssFontFamily::kSansSerif,
                "the generic families index the built-in ones");
  // A built-in body is drawn at the rung a stored size snaps to; another family's sizes are relative to its own.
  const auto rung = CrossPointSettings::builtinRungForPoints(pointSize);
  int familyBody[CrossPointSettings::BUILTIN_FONT_COUNT];
  uint8_t bodyPt = pointSize;
  for (uint8_t family = 0; family < CrossPointSettings::BUILTIN_FONT_COUNT; ++family) {
    familyBody[family] = CrossPointSettings::getBuiltinReaderFontId(family, rung);
    if (familyBody[family] == bodyFontId) bodyPt = CrossPointSettings::fontSizePoints(rung);
  }
  for (uint8_t family = 0; family < CrossPointSettings::BUILTIN_FONT_COUNT; ++family) {
    if ((book->body & CssFontFamily::kGenericMask) == family + 1u || familyBody[family] == bodyFontId) continue;
    families.add(family + 1u, familyBody[family], builtinFontSizeLadder(family, bodyPt));
  }
  for (uint8_t n = 0; n < BookFonts::kMaxAccents; ++n) {
    const char* accent = sdFontSystem.accentFamilyNameFor(cachePath, n);
    const int accentId = accent[0] != '\0' ? resolveSdCardFontId(accent, pointSize) : 0;
    if (const auto native = accentId != 0 ? sdFontSystem.ttfNativeSizes(accentId) : std::nullopt) {
      FontSizeLadder ladder;
      ladder.setNative(native->base, native->bodyPpem, native->minPx, native->maxPx);
      families.add(book->accents[n], accentId, ladder);
    }
  }
}

std::optional<int> EpubReaderActivity::NavigationTarget::relaidOutRestorePage(const Section& sec,
                                                                              const int spineIndex) const {
  if (restoreParagraph == 0 || cachedSpineIdx != spineIndex || cachedPageCount <= 0 ||
      sec.pageCount == cachedPageCount) {
    return std::nullopt;
  }
  const auto p = sec.getPageForParagraphIndex(restoreParagraph);
  if (!p) return std::nullopt;
  LOG_DBG("ERS", "Chapter relaid out (%d -> %d pages): restoring p[%u] -> page %u", cachedPageCount, sec.pageCount,
          restoreParagraph, *p);
  return *p;
}

void EpubReaderActivity::NavigationTarget::resolveInto(Section& sec, int spineIndex) const {
  // Resolve to a baseline page first. Each branch records whether it produced a
  // precise page (LUT/anchor hit, percent jump, explicit page) or only an estimate.
  // The estimate path runs cross-spine rescale + clamp at the end; the precise path
  // skips both because LUT pages are already in the target spine's coordinate system.
  bool isEstimate = false;

  switch (kind) {
    case Kind::LastPage: {
      sec.currentPage = (sec.pageCount > 0) ? sec.pageCount - 1 : 0;
      break;
    }

    case Kind::TocIndex: {
      if (const auto p = sec.getPageForTocIndex(tocIndex)) {
        sec.currentPage = *p;
      }
      break;
    }

    case Kind::Anchor: {
      if (const auto p = sec.getPageForAnchor(anchorStr)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved anchor '%s' -> page %d", anchorStr.c_str(), *p);
      } else if (sec.isTruncatedCache()) {
        // The chapter did not finish building, so its anchor map stops where the parse stopped and
        // a miss says nothing about whether the id exists. Distinct log level because the two
        // cases want opposite fixes: a genuine miss is a bad href in the book, this one is the
        // build running out of heap -- and the destination below is then meaningless rather than
        // merely approximate. Device 2026-09-09: a note 46% into a 90-page notes chapter that had
        // built 31 pages landed the reader on page 0.
        LOG_ERR("ERS", "Anchor '%s' missing from a TRUNCATED spine cache (%u pages built); the jump is a guess",
                anchorStr.c_str(), static_cast<uint32_t>(sec.pageCount));
        sec.currentPage = fallbackPage;
        isEstimate = true;
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found; using fallback page %d", anchorStr.c_str(), fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::ListItem: {
      if (const auto p = sec.getPageForListItemIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved li[%u] -> page %d", lutIndex, *p);
      } else if (const auto pp = sec.getPageForParagraphIndex(lutIndex)) {
        // Some <li>-anchored XPaths land in books where the LI LUT is empty (no <li>
        // inside <body>'s direct children, or all <li>s skipped). Fall back to the
        // paragraph LUT — the running indices coincide often enough to help, and
        // it's strictly better than dropping back to the estimate.
        sec.currentPage = *pp;
        LOG_DBG("ERS", "Li LUT miss for li[%u]; paragraph LUT -> page %d", lutIndex, *pp);
      } else {
        LOG_DBG("ERS", "Li[%u] not in LUT; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::VisibleOffset: {
      if (const auto p = sec.getPageForVisibleTextOffset(visibleOffset)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved offset %u -> page %d", visibleOffset, *p);
      } else {
        LOG_DBG("ERS", "No offset LUT for offset %u; using fallback page %d", visibleOffset, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Paragraph: {
      if (const auto p = sec.getPageForParagraphIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved p[%u] -> page %d", lutIndex, *p);
      } else {
        LOG_DBG("ERS", "Paragraph LUT miss for p[%u]; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Percent: {
      if (sec.pageCount > 0) {
        int newPage = static_cast<int>(spineProgress * static_cast<float>(sec.pageCount));
        if (newPage >= sec.pageCount) newPage = sec.pageCount - 1;
        sec.currentPage = newPage;
      }
      break;
    }

    case Kind::Page: {
      sec.currentPage = page;
      isEstimate = true;
      if (const auto p = relaidOutRestorePage(sec, spineIndex)) {
        sec.currentPage = *p;
        isEstimate = false;
      }
      break;
    }
  }

  // Cross-font / cross-spine rescaling: only for estimated pages. cachedPageCount
  // is the page count at the time the estimate was made — when it disagrees with
  // the section's current page count (reflow / different spine entirely), rescale
  // the estimate proportionally before clamping.
  if (isEstimate && cachedPageCount > 0 && cachedSpineIdx == spineIndex && sec.pageCount != cachedPageCount) {
    const float progress = static_cast<float>(sec.currentPage) / static_cast<float>(cachedPageCount);
    sec.currentPage = static_cast<int>(progress * static_cast<float>(sec.pageCount));
  }

  // Safety clamp for all paths — a LUT-derived page is also defensively clamped in
  // case the cache is somehow stale.
  if (sec.currentPage < 0) {
    LOG_DBG("ERS", "Clamping negative page %d to 0 (spine=%d cachedPageCount=%d)", sec.currentPage, spineIndex,
            cachedPageCount);
    sec.currentPage = 0;
  }
  if (sec.pageCount > 0 && sec.currentPage >= sec.pageCount) {
    LOG_DBG("ERS", "Clamping page %d to last page %d", sec.currentPage, sec.pageCount - 1);
    sec.currentPage = sec.pageCount - 1;
  }
}

// navTarget is the anchor a section (re)build resolves into. It used to be written only when a
// section was ENTERED and then left frozen for the whole chapter, so every mid-chapter teardown
// re-seeded currentPage from the entry page — page 0 for a chapter reached by reading forward.
// The reader landed back at the top of the chapter, and the next render persisted that to
// progress.bin (issue #147). Four teardowns can fire during ordinary reading:
//
//   - fallbackToReleasedRebuild(): a Background-C build aborting mid-build on low heap, or
//     finishing truncated / CSS-degraded — the common one, since the reader is turning pages
//     through a live build the whole time it runs;
//   - renderNormalPass(): loadPageFromSectionFile() returning null (deserialize OOM, bad seek);
//   - both footnote-preview gather triggers, which drop the section to rebuild it previews-ON.
//
// None of them can be expected to remember the position individually, so keep the anchor live
// instead: after this call navTarget names what is on screen, and any teardown lands back there.
void EpubReaderActivity::anchorNavTargetToCurrentPage() {
  if (!section) {
    return;
  }
  navTarget = NavigationTarget::makePage(section->currentPage);
  // Carrying the page count lets resolveInto() rescale proportionally when the rebuild
  // repaginates rather than landing on a raw index — the previews-ON variant does exactly
  // that (it bakes preview text into the layout). Only meaningful once the count is final:
  // during a build pageCount is "pages written so far", and rescaling a partial count
  // against the finished one would throw the position far past where the reader was.
  if (!section->hasActiveBuild() && section->pageCount > 0) {
    navTarget.cachedPageCount = section->pageCount;
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
}

bool EpubReaderActivity::stepPageState(const bool isForwardTurn) {
  RenderLock lock(*this);
  return stepPageStateLocked(isForwardTurn);
}

bool EpubReaderActivity::stepPageStateLocked(const bool isForwardTurn) {
  if (!epub || !section) {
    return false;
  }

  // Background-C in progress: the final page count isn't known yet, so navigate optimistically.
  // Only an explicit Page target has a meaningful position before completion (other targets are
  // resolved when the build finishes); for those, swallow the turn. Forward advances the display
  // cursor (the SectionBuilding pass shows the page once C builds it, or the popup until then);
  // running past the real end is reconciled at completion (clamp / cross to the next spine). Back
  // past page 0 leaves the chapter, aborting the in-flight build via ~Section.
  if (section->hasActiveBuild()) {
    if (navTarget.kind != NavigationTarget::Kind::Page) {
      return false;
    }
    if (isForwardTurn) {
      section->currentPage++;
    } else if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
    // Persist mid-build turns too: the SectionBuilding pass never arms pendingProgressSave, so
    // without this a sleep/power-off during a long build resumes at the build-entry page (issue
    // #75). pageCount 0 = "unknown until completion" — the loader skips rescaling for it and the
    // first post-build render overwrites with the real count. Skipped when the back-cross branch
    // reset the section (position resolves when the previous spine loads).
    if (section) {
      // Mid-build turns are exactly the case the anchor was losing: a build that then falls
      // back to the released path restarts this chapter from navTarget.
      anchorNavTargetToCurrentPage();
      pendingProgressSave.spineIndex = currentSpineIndex;
      pendingProgressSave.page = section->currentPage;
      pendingProgressSave.pageCount = 0;
      pendingProgressSave.pending.store(true, std::memory_order_release);
    }
    lastPageTurnTime = millis();
    forceLoadLargeImages = false;
    pageHasPlaceholders = false;
    return true;
  }

  // A 0-page section (permanently unparse-able chapter) has no within-chapter navigation,
  // but the user must still be able to cross spine boundaries to escape it.
  const bool hasPages = section->pageCount > 0;

  // NOTE: section changes served from cache or a completed Background-B build never release
  // the secondary buffer, so RED RAM baseline is intact and the first page uses a normal fast refresh.
  // When the secondary buffer IS released+reallocated (indexing path, image-warm pass, OOM recovery),
  // the release site immediately calls syncRedRamFromFrameBuffer() to restore the correct baseline.
  if (isForwardTurn) {
    if (hasPages && section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      navTarget = NavigationTarget::makePage(0);
      currentSpineIndex++;
      section.reset();
    } else if (currentSpineIndex + 1 == epub->getSpineItemsCount()) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex++;
      section.reset();
    } else {
      return false;
    }
  } else {
    if (hasPages && section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
  }

  // Only the within-section branches above reach here with a section still loaded; the
  // cross-spine branches set their own target and reset it, and this no-ops for them.
  anchorNavTargetToCurrentPage();
  lastPageTurnTime = millis();
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  return true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // One lock covers cancellation, pre-render selection, navigation, and cleanup. The
  // render task temporarily mutates section/pre-render state and publishes deferred-AA
  // shared_ptrs, so even guard-only reads must be serialized.
  RenderLock lock(*this);

  // Cancel any pending deferred AA pass — it belongs to the page we're leaving.
  pendingGrayscale_ = {};

  // NOTE: Background-B's borrowed framebuffer is deliberately NOT reclaimed here. This runs on
  // the loop task without the render lock, and handing a framebuffer back from under the render
  // task is not something a flag assignment's worth of racing tolerates. The reclaim belongs to
  // recoverSecondaryBufferIfNeeded(), which render() calls before any pass draws — early enough
  // that this page turn still renders with AA, and serialised by the lock render() already holds.

  // If the "Indexing..." popup is currently on screen and the user turns the page/section now, the
  // destination page replaces a dark popup box on the X4 baseline. Whether it lands via a now-built
  // page (displayBuildPage) or by abandoning the build to an adjacent cached section (renderContents
  // Normal pass), a FAST diff against the popup frame leaves a ghost outline. Arm the post-popup HALF
  // so the replacing page establishes a clean baseline. This is a deliberate navigation away from the
  // popup — NOT the routine forward-reading crossing the cold-open/deliberate-jump gating protects —
  // so it doesn't reintroduce the "every section traversal pays a slow refresh" cost. X3's fast
  // differential reads the controller's DTM1 (drawPopup updated it correctly), so it never ghosts.
  if (renderer.fastRefreshNeedsHostFrame() && buildingPopupShown_) {
    forceHalfRefreshAfterPopup_ = true;
  }

  auto logPageTurnWindowIfReady = [this]() {
    if (pageTurnStatsWindow.turns < PAGE_TURN_STATS_WINDOW_SIZE) {
      return;
    }
    const unsigned long hitRatePct =
        (static_cast<unsigned long>(pageTurnStatsWindow.preRenderHits) * 100UL) / pageTurnStatsWindow.turns;
    const unsigned long avgPreRenderMs =
        pageTurnStatsWindow.preRenderHits > 0
            ? (pageTurnStatsWindow.totalPreRenderMs / pageTurnStatsWindow.preRenderHits)
            : 0UL;
    const unsigned long avgIdleSlackMs =
        pageTurnStatsWindow.preRenderHits > 0
            ? (pageTurnStatsWindow.totalIdleSlackMs / pageTurnStatsWindow.preRenderHits)
            : 0UL;
    const uint16_t preRenderMisses = pageTurnStatsWindow.preRenderMisses;
    LOG_DBG("ERS",
            "PageTurn agg(%u): turns=%u prerenderHits=%u prerenderMisses=%u hitRatePct=%lu avgPreRenderMs=%lu "
            "avgIdleSlackMs=%lu",
            PAGE_TURN_STATS_WINDOW_SIZE, pageTurnStatsWindow.turns, pageTurnStatsWindow.preRenderHits, preRenderMisses,
            hitRatePct, avgPreRenderMs, avgIdleSlackMs);
    pageTurnStatsWindow = {};
  };

  const bool hadPreRenderedCandidate =
      isForwardTurn && section && preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex;
  const int expectedNextPage = (section ? section->currentPage + 1 : -1);

  if (isForwardTurn && section && preRenderedPage.ready && !preRenderedPage.incomplete &&
      preRenderedPage.spineIndex == currentSpineIndex && preRenderedPage.pageIndex == section->currentPage + 1) {
    // Fast path: the frame buffer already holds the next page content. Advance state here on the
    // loop task, then hand off to render() via usePreRenderedBuffer — all display work (status
    // bar, flush, AA pass) stays on the render task where it belongs.
    //
    const unsigned long nowMs = millis();
    const unsigned long idleSlackMs = (preRenderedPage.completedAtMs > 0 && nowMs >= preRenderedPage.completedAtMs)
                                          ? (nowMs - preRenderedPage.completedAtMs)
                                          : 0UL;
    LOG_DBG("ERS", "PageTurn stats: prerendered=1 preRenderMs=%lu idleSlackMs=%lu", preRenderedPage.renderDurationMs,
            idleSlackMs);
    pageTurnStatsWindow.turns++;
    pageTurnStatsWindow.preRenderHits++;
    pageTurnStatsWindow.totalPreRenderMs += preRenderedPage.renderDurationMs;
    pageTurnStatsWindow.totalIdleSlackMs += idleSlackMs;
    LOG_DBG("ERS", "PageTurn summary: hit=1 window=%u/%u nextPage=%d", pageTurnStatsWindow.preRenderHits,
            pageTurnStatsWindow.turns, preRenderedPage.pageIndex);
    logPageTurnWindowIfReady();
    section->currentPage = preRenderedPage.pageIndex;
    // This fast path advances the page without going through stepPageState(), so it owes the
    // anchor update too — otherwise every pre-rendered turn (the common case) leaves navTarget
    // behind on the page the section was entered at.
    anchorNavTargetToCurrentPage();
    // ready=false here means CONSUMED, not invalidated: usePreRenderedBuffer
    // below hands this very page to the display. So the staged planes must
    // SURVIVE — they belong to the page about to be shown, and the display is
    // what clears them. Every other site that clears ready is discarding the
    // buffer, and there the planes must go with it.
    preRenderedPage.ready = false;
    usePreRenderedBuffer = true;
    sessionPagesAdvanced++;
    globalReadingSessionTracker().onPageTurn();
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }

  if (!stepPageStateLocked(isForwardTurn)) {
    return;
  }

  LOG_DBG("ERS", "PageTurn stats: prerendered=0 candidate=%d expectedNext=%d cachedNext=%d pendingPreRender=%d",
          hadPreRenderedCandidate ? 1 : 0, expectedNextPage, preRenderedPage.pageIndex, pendingPreRender ? 1 : 0);
  pageTurnStatsWindow.turns++;
  pageTurnStatsWindow.preRenderMisses++;
  LOG_DBG("ERS", "PageTurn summary: hit=0 window=%u/%u expectedNext=%d", pageTurnStatsWindow.preRenderHits,
          pageTurnStatsWindow.turns, expectedNextPage);
  logPageTurnWindowIfReady();

  sessionPagesAdvanced++;
  globalReadingSessionTracker().onPageTurn();
  // Page state advanced without using a pre-render. Drop any pre-render that was
  // scheduled for the page we just left: otherwise the coalesced render() would
  // classify as a PreRender pass and try to pre-render the *new* current page's
  // next page instead of displaying the page we just navigated to — leaving the
  // previous (now stale) frame on screen. (This is the classic last-page case:
  // turning onto the final page would otherwise show the penultimate page.)
  preRenderedPage.ready = false;
  preRenderedPlanesStaged_ = false;
  pendingPreRender = false;
  requestUpdate();
}

void EpubReaderActivity::jumpPages(const bool isForwardTurn, const int count) {
  // One lock for the whole jump, like pageTurn(): the render task publishes the deferred-AA page
  // and the pre-render flags, and a jump that lands in a render's unlocked waveform window must
  // not interleave with that.
  RenderLock lock(*this);
  // The same hand-off pageTurn() makes on its non-pre-rendered path. Without it the jump's render
  // request reached render() with the old page's deferred AA still owed and that page's
  // pre-render still armed, was classified as a PreRender and shelved ("PreRender deferred: AA
  // owed"); the AA then ran on the page being left and the new page was never drawn. X3,
  // 2026-09-26: a double-click during a slow image-page render saved page 144 as the position
  // while the screen stayed on 134 until something else repainted it.
  pendingGrayscale_ = {};
  int stepped = 0;
  while (stepped < count && stepPageStateLocked(isForwardTurn)) {
    ++stepped;
  }
  if (stepped == 0) {
    return;
  }
  preRenderedPage.ready = false;
  preRenderedPlanesStaged_ = false;
  pendingPreRender = false;
  requestUpdate();
}

bool EpubReaderActivity::reallocSecondaryEvictingCaches() {
  // The FDC page slots are per-page state (every prewarmed render batch-clears and refills
  // them via endScanAndPrewarm), but a render done mid-released-build leaves the last page's
  // ~4 KB slot buffers allocated — frequently inside the released-framebuffer hole this
  // realloc is about to ask back as one contiguous block. Dropping them first is free (the
  // next render re-prewarms regardless) and deterministic, so do it before the first attempt.
  if (FontCacheManager* fontCache = renderer.getFontCacheManager()) {
    fontCache->clearCache();
  }
  if (renderer.reallocSecondaryBuffer()) {
    return true;
  }
  // Still blocked: evict the CSS resolve caches (hot/negative caches, container bucket
  // arrays and the retained selector index all reload lazily from SD, worst case ~240 ms
  // at the next section-build start) and drop Background-B's section — an in-flight B
  // build holds a parser plus its inflate ring, and even a settled one keeps LUT/TOC
  // state; B re-probes on its own cadence, so the only cost is redoing lookahead work.
  // All far cheaper than the recovery reboot this call stands between. Retry once.
  if (epub && epub->getCssParser()) {
    epub->getCssParser()->clearCaches(/*evictEverything=*/true);
  }
  if (backgroundSection_) {
    LOG_INF("ERS", "Dropping Background-B section (spine=%d) for secondary realloc", backgroundBuildSpineIndex_);
    resetBackgroundBuild();
  }
  // The image manifest's record array and its kept resolve handle are allocated during a build
  // -- after a released one, inside the very hole this realloc needs (X3 2026-09-25: the then
  // per-image "OEBPS/images/..." path strings were found bounding the freed region; manifest v5
  // replaced them with 12-byte records, which grow in the same place). Persist and drop it, then
  // reload it from images.bin once the buffer is placed. Only with no build holding it: the
  // parser caches the manifest pointer at setup, and the reload replaces the object.
  const bool evictManifest = epub && !(section && section->hasActiveBuild());
  if (evictManifest) epub->releaseImageManifest();
  const bool restored = renderer.reallocSecondaryBuffer();
  if (evictManifest) epub->loadImageManifest();
  if (restored) {
    LOG_INF("ERS", "Secondary realloc succeeded after cache eviction");
    return true;
  }
  return false;
}

EpubReaderActivity::PageLinkInfo EpubReaderActivity::pageLinkInfoForCurrentPage() {
  PageLinkInfo info;
  info.previews.resize(currentPageFootnotes.size());
  // Everything is a note until the store says otherwise: a book with no store must look exactly
  // as it did before any of this. The distinction only groups the list -- following a link always
  // saves a return position either way (see navigateToHref).
  info.isNote.assign(currentPageFootnotes.size(), 1);
  if (!epub) {
    return info;
  }
  // Under the render lock because resolving a cross-file note href walks the spine through
  // BookMetadataCache, which seeks and reads a book.bin handle whose position is SHARED with
  // renderStatusBar() on the render task (calculateProgress -> getSpineItem -> getSpineEntry).
  // This runs from the reader-menu result handler, which the ActivityManager dispatches with
  // the lock released, so without this the two interleave on one file position and a spine
  // entry comes back as whatever the other task had seeked to. Cheap to hold: Lookup memoises
  // the path, so a page's notes cost one spine walk, not one per note.
  //
  // Taken here rather than by the callers because both of them (the menu item and the
  // BTN_FOOTNOTES button action) reach this on the loop task with no lock held.
  RenderLock lock(*this);
  // Purely a read. Whatever the reader has walked through has already resolved its notes at
  // build time, so the entries for this page are in the store; a link the store does not know
  // renders as its plain marker and stays navigable.
  //
  // The same lookup answers both questions, because they are the same question: the store holds
  // a note's text keyed by its caller's href, so a hit IS "this is a note" and the text is the
  // preview. A miss on a spine the store HAS scanned means the link is navigation -- a contents
  // link, a cross-reference -- and the list groups it separately.
  if (!FootnotePreviews::spineResolved(epub->getCachePath(), currentSpineIndex)) {
    return info;
  }
  FootnotePreviews::Lookup previewLookup;
  if (!previewLookup.open(epub->getCachePath(), epub.get(), currentSpineIndex)) {
    return info;
  }
  for (size_t i = 0; i < currentPageFootnotes.size(); ++i) {
    info.isNote[i] = previewLookup.find(currentPageFootnotes[i].href, info.previews[i]) ? 1 : 0;
  }
  return info;
}

bool EpubReaderActivity::nearChapterEndForBuildThrough() const {
  if (renderer.fastRefreshNeedsHostFrame() || !section || section->hasActiveBuild() || section->pageCount == 0)
    return false;
  return static_cast<int>(section->pageCount) - 1 - section->currentPage <= BG_BUILD_THROUGH_PAGES;
}

void EpubReaderActivity::recoverSecondaryBufferIfNeeded() {
  // Build-through (BG_BUILD_THROUGH_PAGES): B is building the next chapter and the reader is
  // finishing this one, so the build keeps the buffer and this page draws without it. Engaged by
  // the first render that finds the reader that close, when B did not already start in that mode.
  // Also kept when the reader has just turned INTO that chapter, so buildSection() adopts the
  // live build instead of starting it over. Any other move takes the buffer back as below.
  if (backgroundBorrowActive_ && backgroundSection_ && backgroundSection_->hasActiveBuild()) {
    const bool nextChapter = backgroundBuildSpineIndex_ == currentSpineIndex + 1;
    if (!backgroundBuildThrough_ && nextChapter && nearChapterEndForBuildThrough()) {
      backgroundBuildThrough_ = true;
      LOG_INF("ERS", "Background-B: page %d of %u reached mid-build; building spine %d through page turns",
              section->currentPage + 1, section->pageCount, backgroundBuildSpineIndex_);
    }
    if (backgroundBuildThrough_ && (nextChapter || backgroundBuildSpineIndex_ == currentSpineIndex)) {
      return;
    }
  }
  // A render is about to draw page content, and AA is gated on a RESIDENT secondary buffer
  // (see renderContents). If Background-B is building inside the borrowed one, take it back
  // first — otherwise this frame silently renders BW. No-op unless B holds it, which is the
  // normal case: page turns already hand it back in pageTurn(), so what reaches here is the
  // occasional battery/clock-tick Normal render. Deliberately ahead of the Background-C guard
  // below: C's borrow serves a build the reader is watching and must survive; B's is
  // look-ahead that yields to anything the reader can see.
  endBackgroundBorrow();
  // While Background-C is building with the buffer released for headroom, leave it released —
  // reallocating now would reclaim the ~48–52 KB the build is using. The buffer is restored here
  // on the first render after the build ends (hasActiveBuild() goes false).
  if (section && section->hasActiveBuild()) {
    return;
  }
  // Borrowed-buffer path: the build ran inside the LENT secondary framebuffer, so return it
  // instead of reallocating. The region never entered the heap, so returnSecondaryBuffer()
  // cannot fail — no realloc/eviction/forensics/heap-recovery needed here. Drop the section's
  // reference to the scratch arena first, then free the (small, non-owning) arena object, then
  // hand the block back to the display.
  if (secondaryBorrowed_ && !renderer.hasSecondaryBuffer()) {
    if (section) section->setExternalBuildScratch(nullptr);
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();
    secondaryBorrowed_ = false;
    secondaryBufferDegraded_ = false;
    renderer.setSingleBufferFastDiff(false);
    LOG_INF("ERS", "Secondary display buffer returned (borrow); re-enabling normal refresh/AA paths");
    return;
  }
  // Opportunistic recovery: after an OOM during chapter indexing, or after a Background-C
  // released build, restore the secondary buffer when heap is healthy again.
  if (secondaryBufferDegraded_ && !renderer.hasSecondaryBuffer()) {
    if (reallocSecondaryEvictingCaches()) {
      secondaryBufferDegraded_ = false;
      // Undo the IncrementalReleased opt-in (see chooseSectionBuildMode/buildSection): once the
      // secondary buffer is back, double-buffer fast-diff is correct again and this flag would
      // otherwise leave RED RAM reseeding skipped on the normal path. No-op if it was never set
      // (e.g. recovering from an indexing OOM instead of a released build).
      renderer.setSingleBufferFastDiff(false);
      // Do NOT syncRedRamFromFrameBuffer() here: reallocSecondaryBuffer() whitened the new secondary,
      // and syncRedRamFromFrameBuffer() would copy that white into RED RAM, destroying the baseline.
      // RED already holds the last displayed page (kept current by the released build's FAST refreshes;
      // the controller retains it through realloc). Reseeding from white ghosted the next page.
      LOG_INF("ERS", "Secondary display buffer restored; re-enabling normal refresh/AA paths");
    } else {
      const uint32_t freeHeap = esp_get_free_heap_size();
      // Safe to walk the heap here (unlike the post-index OOM path): this is a routine
      // render-start recovery, not the aftermath of decode failures under pressure.
      const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
      LOG_ERR("ERS", "Secondary display buffer realloc failed (free=%lu contig=%lu); AA stays off, will retry",
              freeHeap, contigHeap);
      // One-shot forensics so field logs identify WHAT is pinning the released hole. Once per
      // boot: the block map barely changes between failed retries.
      static bool dumpedHeapOnce = false;
      if (!dumpedHeapOnce) {
        dumpedHeapOnce = true;
        LOG_ERR("ERS", "Heap pin forensics (one-shot, free spans >= 1 KB and their neighbours):");
        logHeapPinForensics();
      }
      // Cache eviction plus opportunistic retries did not recover a framebuffer-sized
      // contiguous block, so escalate to a recovery reboot once free heap is plentiful
      // but the block still can't be found.
      maybeRestartForFragmentedHeap(freeHeap, contigHeap);
    }
  } else if (secondaryBufferDegraded_ && renderer.hasSecondaryBuffer()) {
    secondaryBufferDegraded_ = false;
    renderer.setSingleBufferFastDiff(false);
  }
}

void EpubReaderActivity::clampSpineIndex(const int spineCount) {
  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen (spineCount is the finished-book sentinel)
  if (currentSpineIndex > spineCount) {
    currentSpineIndex = spineCount;
  }
}

EpubReaderActivity::RenderLayout EpubReaderActivity::computeRenderLayout() const {
  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);

  const int bezelLeft = orientedMarginLeft, bezelRight = orientedMarginRight;
  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  // One-shot per distinct result. The bezel inset is only one of two terms here:
  // SETTINGS.screenMargin is added on top, so a few pixels of inset change can be
  // invisible next to a large user margin. Logging both terms and the total says
  // which one actually governs the edge the reader sees.
  static int lastMarginKey = -1;
  const int marginKey = (orientedMarginLeft << 16) | (orientedMarginRight << 8) | SETTINGS.screenMargin;
  if (marginKey != lastMarginKey) {
    lastMarginKey = marginKey;
    LOG_INF("ERS", "Reader margins: bezel L%d R%d + screenMargin %d = L%d R%d (screen %dx%d)", bezelLeft, bezelRight,
            static_cast<int>(SETTINGS.screenMargin), orientedMarginLeft, orientedMarginRight, renderer.getScreenWidth(),
            renderer.getScreenHeight());
  }

  RenderLayout layout;
  layout.marginTop = orientedMarginTop;
  layout.marginRight = orientedMarginRight;
  layout.marginBottom = orientedMarginBottom;
  layout.marginLeft = orientedMarginLeft;
  layout.viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  layout.viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  return layout;
}

bool EpubReaderActivity::dropStagedWorkIfPositionMoved(const char* where) {
  if (!pendingGrayscale_.active && !pendingPreRender) return false;
  // pageTurn()'s pre-rendered fast path moves the position on purpose and made the hand-off
  // itself; the buffer it hands over is the new page.
  if (usePreRenderedBuffer) return false;
  const int page = section ? section->currentPage : -1;
  if (currentSpineIndex == stagedForSpine_ && page == stagedForPage_) return false;
  LOG_DBG("ERS", "%s: position moved (spine %d page %d -> spine %d page %d); dropping the%s%s of the page left", where,
          stagedForSpine_, stagedForPage_, currentSpineIndex, page, pendingGrayscale_.active ? " deferred AA" : "",
          pendingPreRender ? " pre-render" : "");
  pendingGrayscale_ = {};
  pendingPreRender = false;
  preRenderedPage.ready = false;
  preRenderedPlanesStaged_ = false;
  return true;
}

EpubReaderActivity::RenderPass EpubReaderActivity::classifyRenderPass() const {
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return RenderPass::FinishedBook;
  }
  // Background-C owns the screen while the current section is being built: draw the requested
  // page from the in-progress LUT, or the indexing popup. Checked ahead of the pre-render /
  // buffer-display passes (which never arm during a build) so a build can never be pre-empted.
  if (section && section->hasActiveBuild()) {
    return RenderPass::SectionBuilding;
  }
  // BufferDisplay is checked before PreRender to preserve the former in-line order
  // (the buffer-display block ran ahead of the pre-render block). It applies only when
  // the prior waveform + post-waveform SPI work has settled; otherwise we fall through
  // to a full render that waits for the waveform naturally. The helper may still bail to
  // Normal at runtime if the page load fails / is an image page.
  if (usePreRenderedBuffer && !renderer.isRefreshPending()) {
    return RenderPass::BufferDisplay;
  }
  if (pendingPreRender) {
    return RenderPass::PreRender;
  }
  if (!section) {
    return RenderPass::BuildSection;
  }
  return RenderPass::Normal;
}

void EpubReaderActivity::renderFinishedBookPass(const int spineCount) {
  // Immediately transition to finished-book flow instead of showing an end-of-book screen
  if (finishedBookActivityStarted_) {
    return;
  }
  finishedBookActivityStarted_ = true;
  const int lastSpineIndex = std::max(0, spineCount - 1);
  writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
  finishedBookSyncSpineIndex_ = lastSpineIndex;
  finishedBookSyncPage_ = 0;
  finishedBookSyncPageCount_ = 0;
  // Arm only; loop() performs the launch on the loop task (see finishedBookLaunchPending_).
  // This pass used to drop the render lock and call launchFinishedBookFlow() from here, which
  // both mutated ActivityManager's pending-activity state from the wrong task and did SD work
  // (findNextBookInDirectory) on the render task's stack. The lock is now simply kept: nothing
  // below it is long-running any more.
  finishedBookLaunchPending_.store(true, std::memory_order_release);
}

void EpubReaderActivity::serviceFinishedBookLaunch() {
  if (!finishedBookLaunchPending_.load(std::memory_order_acquire) || !epub) {
    return;
  }
  finishedBookLaunchPending_.store(false, std::memory_order_relaxed);
  BookFinished::launchFinishedBookFlow(
      *this, renderer, mappedInput, epub->getPath(), epub->getSeries(), epub->getSeriesIndex(), epub->getAuthor(),
      [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->finishedBookActivityStarted_ = false; }, this,
      &EpubReaderActivity::onFinishedBookSyncRequested, this);
}

bool EpubReaderActivity::renderBufferDisplayPass(const RenderLayout& layout) {
  // Fast-display pass: frame buffer holds pre-rendered content; superimpose live status bar,
  // flush to display, and run the AA pass — all on the render task with no SD font re-read.
  if (!section) {
    return false;
  }
  auto p = section->loadPageFromSectionFile();
  if (!p) {
    // Page load failed — caller falls through to full render.
    return false;
  }
  // Same reason as in renderContents(): pin the page identity before the pass does any work.
  lastRenderedSpineIndex_ = currentSpineIndex;
  lastRenderedPageIndex_ = section->currentPage;
  lastRenderedPageCount_ = section->pageCount;
  currentPageFootnotes = std::move(p->footnotes);
  displayPreRenderedPage(*p, layout.marginTop, layout.marginRight, layout.marginBottom, layout.marginLeft);

  pendingProgressSave.spineIndex = currentSpineIndex;
  pendingProgressSave.page = section->currentPage;
  pendingProgressSave.pageCount = section->pageCount;
  pendingProgressSave.pending.store(true, std::memory_order_release);

  // Not during a build-through: the X3's frame buffer is the only one, and it must keep the page
  // on screen until the buffer goes back (see BG_BUILD_THROUGH_PAGES).
  if (!backgroundBuildThrough_ && section->currentPage + 1 < section->pageCount) {
    pendingPreRender = true;
    markStagedForCurrentPage();
    requestUpdate();
  }
  LOG_DBG("ERS", "Page summary: spine=%d page=%d/%d prerendered=1 refresh=%s mode=0x%02X", currentSpineIndex,
          lastRenderedPageIndex_, lastRenderedPageCount_, refreshModeName(lastPageRefreshMode_),
          lastPageDisplayModeByte_);
  return true;
}

void EpubReaderActivity::renderPreRenderPass(const RenderLayout& layout) {
  // Pre-render pass: render next page content into the frame buffer (no status bar, no flush).
  if (!section || preRenderedPage.ready || backgroundWorkSuspended_ || backgroundBuildThrough_) {
    return;
  }
  const int nextPage = section->currentPage + 1;
  // During an active build use the in-memory LUT via loadPageFromActiveBuild; the on-disk
  // LUT is not written until finalisation so loadPageFromSectionFile would always miss.
  const bool buildActive = section->hasActiveBuild();
  const int availablePages =
      buildActive ? static_cast<int>(section->activeBuildPageCount()) : static_cast<int>(section->pageCount);
  // Both refusals below used to be silent, which cost a whole debugging session: an X3
  // sitting just under the heap floor disabled Background A for every page of a run with
  // nothing in the log to distinguish "declined" from "never scheduled". They log at DBG
  // and fire at most once per requested pass, so they cannot spam.
  if (nextPage >= availablePages) {
    LOG_DBG("ERS", "PreRender skipped: next=%d >= available=%d (buildActive=%d)", nextPage, availablePages,
            buildActive ? 1 : 0);
    return;
  }
  const uint32_t freeHeap = esp_get_free_heap_size();
  if (freeHeap < PRE_RENDER_MIN_FREE_HEAP_BYTES) {
    LOG_DBG("ERS", "PreRender skipped: free=%lu < floor=%lu", static_cast<unsigned long>(freeHeap),
            static_cast<unsigned long>(PRE_RENDER_MIN_FREE_HEAP_BYTES));
    return;
  }
#if DEBUG_BACKGROUND_WORK
  bgCounters_.aRuns++;
#endif
  // Heap telemetry for the floor above. The three snapshots bracket the only two consumers:
  //   begin -> after_load  = Page::deserialize (elements + TextBlock arenas), the one cost
  //                          the floor was never set from any measurement of;
  //   after_load -> end    = the prewarm buffers, whose own sizes the FDC "[prewarm] mem:"
  //                          line already reports (pageBuf / pageGlyphs / peakTemp).
  // The Page dies with `p` at the end of this function, so `end` also shows what is retained
  // (nothing but the pixels in the framebuffer).
  logReaderMemSnapshot("prerender_begin");
  // What the pass really costs: the heap's low point between here and the end of the draw, the
  // number the floor above has to be derived from. The allocator tracks it from here, the same
  // way endBackgroundBorrow() reads Background B's. B is never live here (render() returns its
  // buffer first), but if the monitor is somehow already running it is left alone.
  const bool trackLow = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
  const int savedPage = section->currentPage;
  section->currentPage = nextPage;
  auto p = buildActive ? section->loadPageFromActiveBuild(static_cast<uint16_t>(nextPage))
                       : section->loadPageFromSectionFile();
  section->currentPage = savedPage;
  logReaderMemSnapshot("prerender_after_load");
  // An image page qualifies once every image on it replays from its pixel caches, which the image
  // lane has usually written pages ahead. Never with a decode: this pass holds the render lock and
  // cannot abort, so a 1-4 s decode would stall the next turn behind it. The greyscale cache is
  // required wherever the display replays it (the lane's warmGrayscale rule), or the images would
  // come up without their grey levels. Not on a single-push panel: its capture rides the glyph
  // blit and records no image greys, so the two-push fallback would show such a page differently
  // from the same page rendered fresh.
  const bool drawn =
      p && (!p->hasImages() ||
            (!renderer.supportsGrayFrame() &&
             !p->hasUncachedImages(/*forceLoadLargeImages=*/true, /*monochromeOutput=*/true,
                                   /*alsoWarmGrayscale=*/getEffectiveTextAntiAliasing() && !secondaryBufferDegraded_)));
  const bool skippedForImages = p && !drawn;
  uint16_t glyphsDropped = 0;
  uint16_t imagesMissed = 0;
  const unsigned long preRenderStart = millis();
  if (drawn) {
    section->currentPage = nextPage;
    {
      // Belt and braces for the check above: a cache that exists can still fail to replay, and
      // the draw would then fall through to the decode this pass must never run.
      ImageBlock::PlaceholderOnlyScope cacheOnly;
      glyphsDropped =
          renderPageContentOnly(*p, layout.marginTop, layout.marginRight, layout.marginBottom, layout.marginLeft);
      imagesMissed = cacheOnly.placeholdersDrawn();
    }
    section->currentPage = savedPage;
    logReaderMemSnapshot("prerender_end");
  }
  const unsigned long preRenderDuration = millis() - preRenderStart;
  p.reset();  // inside the tracked window: the page is part of the pass's cost
  uint32_t lowest = 0;
  if (trackLow) {
    lowest = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
    heap_caps_monitor_local_minimum_free_size_stop();
  }
  if (drawn) {
    // An incomplete draw keeps `ready` (the framebuffer does hold that page now; the sleep
    // overlay and screenshots restore the current one from it) but is never shown: the turn
    // renders afresh, without this pass's Page in the heap.
    const bool incomplete = glyphsDropped > 0 || imagesMissed > 0;
    preRenderedPage = {true, currentSpineIndex, nextPage, preRenderDuration, millis(), incomplete};
#if DEBUG_BACKGROUND_WORK
    if (!incomplete) bgCounters_.aCompletes++;
#endif
    LOG_DBG("ERS", "Pre-rendered page %d/%d in %lums (heap %lu at start, %lu lowest, cost %lu)%s", nextPage,
            section->pageCount - 1, preRenderDuration, static_cast<unsigned long>(freeHeap),
            static_cast<unsigned long>(lowest),
            static_cast<unsigned long>(lowest && freeHeap > lowest ? freeHeap - lowest : 0),
            incomplete ? "; incomplete, will not be shown" : "");
    if (glyphsDropped > 0) {
      LOG_ERR("ERS", "Pre-render of page %d dropped %u glyphs for lack of memory", nextPage, glyphsDropped);
    }
    if (imagesMissed > 0) {
      LOG_INF("ERS", "Pre-render of page %d: %u image(s) did not replay from cache", nextPage, imagesMissed);
    }
  } else if (skippedForImages) {
    LOG_DBG("ERS", "PreRender skipped: page %d has images that would need a decode (or a single-push panel)", nextPage);
  }
  checkHeapIntegrity("after_prerender");
}

bool EpubReaderActivity::heapAllowsInPlaceBuild(const bool embeddedStyle, const size_t inflatedSize) const {
  // Mirror the gate Background-B uses to build with the secondary buffer resident: if a CSS
  // book's selector index won't fit alongside the buffer, don't even attempt the in-place
  // build. Then require a free/contig floor pinned above B's idle gate — the foreground build
  // runs at page-turn time with the secondary buffer (and possibly a pre-rendered page) live.
  if (embeddedStyle) {
    const CssParser* css = epub->getCssParser();
    if (!Section::heapAllowsEmbeddedStyle(css ? css->ruleCount() : 0)) {
      return false;
    }
  }
  // The extraction phase holds an inflate ring sized to the entry (≤32 KB) — a per-spine cost the
  // static floors were never tuned for (they fit the common few-KB chapter, where the ring is
  // noise). Add it to the floors the same way Background-B's WaitHeap gate does (ring on top of
  // the base, contig raised to ring + scratch), so a whole-book-in-one-spine entry is sent
  // straight to the released path instead of starting a resident build that is doomed to the
  // low-heap abort (~1.4 s of popup/setup/abort/re-setup waste, observed heap dip to <8 KB free).
  const uint32_t ringBytes = static_cast<uint32_t>(std::min<size_t>(32768, inflatedSize));
  // max(), not sum: the build's two phases have disjoint peaks (runBuildParse releases the
  // inflate ring before the CSS-resolving parse starts), so the ring and the layout working set
  // are never live at the same time. This is the same shape Background-B's heap-backed gate has
  // always used; only this one summed them.
  //
  // Device-measured on X4 (2026-08-15, HEAP_GATE_TRACE, book cache deleted to force builds):
  //   spine 14  ring=27875  free=62468  old floor=95459   arena high-water 39268/48000, failedAlloc=0
  //   spine 16  ring=32768  free=61580  old floor=100352  arena high-water 44148/48000, failedAlloc=0
  // Total heap on that device is 265120 and free never exceeds ~69 KB at reader time, so the old
  // floor could not be met by any book on any page — it was a constant false, not a tuning. That
  // also made RESIDENT_BUILD_ABORT_* dead code, since reaching it requires IncrementalResident.
  //
  // NOTE the CSS base still (correctly) rejects both of those builds at ~62 KB free: a heap-backed
  // parse there would dip under the runtime CSS-resolve floor (~40 KB) and come out degraded. The
  // fix is not "let more builds go resident" — it is "let this gate be answerable at all". What it
  // changes in practice is the NON-CSS case, where the floor drops from 60 KB + ring (up to 92 KB,
  // unreachable) to a flat 60 KB, which the ~62-69 KB reading heap can actually clear.
  const uint32_t freeFloor =
      std::max<uint32_t>(embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_FREE_HEAP_BYTES : IN_PLACE_BUILD_MIN_FREE_HEAP_BYTES,
                         IN_PLACE_BUILD_EXTRACT_BASE_HEAP_BYTES + ringBytes);
  const uint32_t contigFloor = std::max<uint32_t>(embeddedStyle ? IN_PLACE_BUILD_CSS_MIN_CONTIG_HEAP_BYTES
                                                                : IN_PLACE_BUILD_MIN_CONTIG_HEAP_BYTES,
                                                  ringBytes + 8 * 1024) -
                               LARGEST_FREE_BLOCK_SLACK;
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  const bool ok = freeHeap >= freeFloor && contigHeap >= contigFloor;
  // One line per gate evaluation: the resident/released choice and the exact heap-vs-floor
  // arithmetic behind it. This is the first thing to check when a section build takes the wrong
  // path (a resident build starving on the inflate ring cascades into the released rebuild).
  LOG_INF("ERS", "heapAllowsInPlaceBuild=%d css=%d ring=%lu free=%lu(floor=%lu) contig=%lu(floor=%lu)", ok ? 1 : 0,
          embeddedStyle ? 1 : 0, static_cast<unsigned long>(ringBytes), static_cast<unsigned long>(freeHeap),
          static_cast<unsigned long>(freeFloor), static_cast<unsigned long>(contigHeap),
          static_cast<unsigned long>(contigFloor));
  // Also emit in the shared gate format so one `grep HEAP` collects every gate decision in a run.
  HEAP_GATE(embeddedStyle ? "inPlace_css" : "inPlace", ok, freeHeap, freeFloor, contigHeap, contigFloor);
  return ok;
}

EpubReaderActivity::SectionBuildMode EpubReaderActivity::chooseSectionBuildMode(const bool embeddedStyle,
                                                                                const size_t inflatedSize) const {
  // A failed Background-C attempt latches the old blocking path for this spine.
  if (forceBlockingBuildSpine_ == currentSpineIndex) return SectionBuildMode::Blocking;
  // The secondary buffer is already LENT to a build arena — either Background-B's, just adopted
  // with its build still live, or a prior C borrow. hasSecondaryBuffer() is false in that state,
  // so this must be tested first: falling through would pick Blocking and index the whole section
  // behind the popup while a perfectly good partial build sits in the arena. Released is what the
  // borrow already implements (single-buffer display semantics, headroom in the lent block).
  if (secondaryBorrowed_) return SectionBuildMode::IncrementalReleased;
  // No secondary buffer to keep or release (already degraded): blocking path reallocs it at the end.
  if (!renderer.hasSecondaryBuffer()) return SectionBuildMode::Blocking;
  // A resident build that aborted on the low-heap guard retries released but still INCREMENTAL:
  // the heapAllowsInPlaceBuild floors below already proved optimistic for this spine (they gate on
  // heap at entry, not on what a big spine's ring + parser actually consume), so don't consult
  // them again — and don't collapse to Blocking, which would index the whole section before the
  // first page appears. Checked after the buffer guard: with the buffer already gone the released
  // headroom exists anyway and the blocking path handles the realloc at the end.
  if (forceReleasedBuildSpine_ == currentSpineIndex) return SectionBuildMode::IncrementalReleased;

  // X3 → always build released. Its differential baseline lives in the controller's DTM1, so
  // keeping the ~52 KB RAM buffer resident buys no display benefit (fast refresh works without it)
  // and only starves the build — exactly the foreground policy (inPlace is X4-only). Releasing
  // also keeps CSS parses above the runtime resolve floor (the resident build css-degraded
  // on-device, then had to be rebuilt blocking). Mid-build BW draws still work off DTM1.
  if (!renderer.fastRefreshNeedsHostFrame()) return SectionBuildMode::IncrementalReleased;

  // X4 → keep the secondary buffer resident when the in-place floors fit (fast-refresh baseline
  // re-seeds from it, AA stays live during the build); otherwise release for headroom. This now
  // applies to CSS books too: every build is two-phase, so the inflate ring is released BEFORE the
  // CSS-resolving parse, keeping the resolve clear of the ~40 KB floor when the (higher) CSS
  // in-place floor is met. A resident CSS build that still degrades is caught by
  // isCssLowHeapDegraded() and rebuilt with the buffer released — so this gates "try in place"
  // rather than guaranteeing it. (Historically CSS books always released, from before the build
  // was two-phase, when the ring + parser + resolver were all live at once.)
  return heapAllowsInPlaceBuild(embeddedStyle, inflatedSize) ? SectionBuildMode::IncrementalResident
                                                             : SectionBuildMode::IncrementalReleased;
}

EpubReaderActivity::BuildOutcome EpubReaderActivity::compileSectionCache(const RenderLayout& layout,
                                                                         const bool embeddedStyle,
                                                                         const uint8_t imageRendering) {
  // Use a cleaner waveform for the indexing popup right after image pages; a FAST popup refresh
  // can leave visible bleed on X3 transitioning image -> text.
  if (pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage) {
    renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
    pendingHalfRefreshAfterImagePage = false;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  }
  // Draw the popup BEFORE any secondary-buffer release: drawPopup() → displayBuffer() →
  // swapBuffers(), and with the buffer released frameBufferActive is null, so swapBuffers()
  // would set frameBuffer = null and crash subsequent rendering. Drawing it here also makes the
  // popup the fast-refresh baseline on the in-place path — the panel shows the popup, and the
  // first page diffs popup → page cleanly with no ghosting.
  GUI.drawPopup(renderer, tr(STR_INDEXING));

  // Reset cumulative SD font metadata cache so this section starts fresh.
  renderer.clearFontAccumulation();
  readerPhase_ = ReaderPhase::PRECOMPILING;
  renderer.dropFontMetadata();

  // Build params from the current reader state. embeddedStyle and imageRendering come from
  // the caller (may differ from lastRenderStats on the CSS-retry path); viewport comes from
  // the layout already resolved by the caller.
  Section::BuildParams buildParams = makeSectionBuildParams();
  buildParams.embeddedStyle = embeddedStyle;
  buildParams.imageRendering = imageRendering;
  buildParams.viewportWidth = layout.viewportWidth;
  buildParams.viewportHeight = layout.viewportHeight;

  // Image headers an earlier build could not size (a walk short of memory keeps its image queued
  // in the manifest) are resolved now, from the framebuffer, before this build carves up the
  // heap: whatever it lays out from here on is answered from the manifest with no ring at all.
  if (!secondaryBorrowed_) resolvePendingImageHeadersFromFramebuffer();

  // Prefer to build WITHOUT releasing the secondary buffer when heap is ample, so the chapter's
  // first page keeps a valid fast-refresh baseline. The in-place attempt defers image decode to
  // the lazy per-page path, so a failure here is a graceful parser abort (not a corruption-prone
  // decode under pressure). In-place is X4-only: the X3's baseline lives in the controller, so
  // keeping the RAM buffer resident buys no display benefit, only less headroom.
  //
  // Otherwise the buffer is LENT to the build as its arena, not released (memory audit 2026-09,
  // R1). The lent block never enters the heap, so nothing can pin the hole it would have left
  // and the hand-back at the end cannot fail -- the realloc-failure / heap-recovery-restart
  // class of outcome does not exist on this path. The build gets the Background-C allocation
  // pattern (CSS ruleset, SAX state, feed chunk and inflate ring in the region). Measured on the
  // X3 (Strange Pictures): the reader sits at ~27 KB contig after a borrowed build against
  // 11.8 KB flat after a released one.
  //
  // Release stays for two cases. (1) The Background-C failure latch: C already ran borrowed and
  // failed on the heap, so borrowing again would repeat exactly that; its escalation is the
  // +52 KB of a released build, and it must not be re-decided here (observed on-device: a
  // low-heap abort whose "blocking" retry rebuilt in place ground through the whole spine at
  // <8 KB min free). (2) Nothing to lend: no secondary buffer, or one already lent elsewhere.
  // A borrowed build that fails escalates to a released rebuild the same way (below).
  bool released = false;
  bool borrowedHere = false;  // lent by THIS call, handed back before it returns
  size_t inflatedSize = 0;
  epub->getSpineItemInflatedSize(currentSpineIndex, &inflatedSize);
  const bool escalated = forceBlockingBuildSpine_ == currentSpineIndex;
  const bool inPlace = renderer.fastRefreshNeedsHostFrame() && renderer.hasSecondaryBuffer() && !escalated &&
                       heapAllowsInPlaceBuild(embeddedStyle, inflatedSize);
  const auto lendSecondaryBuffer = [&]() {
    size_t borrowedSize = 0;
    uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize);
    if (!borrowed) return false;
    buildScratch_ = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
    if (!buildScratch_ || !buildScratch_->valid()) {
      buildScratch_.reset();
      renderer.returnSecondaryBuffer();
      return false;
    }
    section->setExternalBuildScratch(buildScratch_.get());
    secondaryBorrowed_ = true;
    secondaryBufferDegraded_ = true;  // AA needs a resident buffer; nothing renders until it is back
    borrowedHere = true;
    return true;
  };
  // Hand a borrow taken above back to the display. The return cannot fail; it re-seeds the
  // baseline exactly like a realloc does.
  const auto handBackBorrow = [&]() {
    section->setExternalBuildScratch(nullptr);
    buildScratch_.reset();
    renderer.returnSecondaryBuffer();
    secondaryBorrowed_ = false;
    secondaryBufferDegraded_ = false;
    borrowedHere = false;
  };
  if (inPlace) {
    LOG_INF("ERS", "Building section in place (secondary buffer kept): free=%lu contig=%lu", esp_get_free_heap_size(),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  } else if (!escalated && !secondaryBorrowed_ && renderer.hasSecondaryBuffer() && lendSecondaryBuffer()) {
    LOG_INF("ERS", "Index start mem (secondary buffer BORROWED as the build arena, %u bytes): free=%lu contig=%lu",
            static_cast<unsigned>(buildScratch_->capacity()), esp_get_free_heap_size(),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  } else {
    LOG_INF("ERS", "Index start mem (before fb release%s): free=%lu contig=%lu", escalated ? ", C-failure latch" : "",
            esp_get_free_heap_size(),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
    renderer.releaseSecondaryBuffer();  // frees ~52 KB for CSS parser + image decoder
    released = true;
    LOG_INF("ERS", "Index start mem (after fb release): free=%lu contig=%lu", esp_get_free_heap_size(),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  }

  // Blocking build (the fallback path: X3, tight heap, a failed Background-C attempt, or a
  // CSS-fallback rebuild). Background-C owns the responsive, build-while-you-read case; here we
  // just build to completion. A resumed partial build continues via the same stepSectionBuild
  // state inside createSectionFile.
  const auto runCreate = [&]() { return section->createSectionFile(buildParams, nullptr, /*skipEviction=*/false); };

  const uint32_t createStart = millis();
  bool createOk = runCreate();
  LOG_INF("ERS", "createSectionFile returned %d in %ums (free=%lu contig=%lu)", createOk ? 1 : 0,
          millis() - createStart, esp_get_free_heap_size(),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  checkHeapIntegrity("after_createSectionFile");

  if (!createOk && inPlace) {
    // The conservative in-place gate was too optimistic. createSectionFile already reset its
    // build state on failure, so the retry starts clean; free the buffer for the headroom the
    // blocking foreground path has always relied on.
    LOG_INF("ERS", "In-place section build failed; retrying with secondary buffer released (free=%lu)",
            esp_get_free_heap_size());
    renderer.releaseSecondaryBuffer();
    released = true;
    const uint32_t retryStart = millis();
    createOk = runCreate();
    LOG_INF("ERS", "createSectionFile retry returned %d in %ums (free=%lu)", createOk ? 1 : 0, millis() - retryStart,
            esp_get_free_heap_size());
    checkHeapIntegrity("after_createSectionFile_retry");
  }
  if (!createOk && borrowedHere) {
    // The borrowed build failed. Say whether the REGION or the heap ran out (failedAllocSize is
    // the last refused arena size; highWater only counts successes), then escalate exactly as
    // Background-C does: hand the block back and rebuild released, with the heap 52 KB bigger.
    LOG_ERR("ERS",
            "Borrowed blocking build spine=%d failed: arena highWater=%u/%u failedAlloc=%u releaseFails=%lu "
            "free=%lu -- retrying released",
            currentSpineIndex, static_cast<uint32_t>(buildScratch_->highWater()),
            static_cast<uint32_t>(buildScratch_->capacity()), static_cast<uint32_t>(buildScratch_->failedAllocSize()),
            static_cast<unsigned long>(buildScratch_->releaseFailures()),
            static_cast<unsigned long>(esp_get_free_heap_size()));
    handBackBorrow();
    renderer.releaseSecondaryBuffer();
    released = true;
    const uint32_t retryStart = millis();
    createOk = runCreate();
    LOG_INF("ERS", "createSectionFile released retry returned %d in %ums (free=%lu)", createOk ? 1 : 0,
            millis() - retryStart, esp_get_free_heap_size());
    checkHeapIntegrity("after_createSectionFile_released_retry");
  }

  // An image dropped to alt text because its header walk was deferred (#249) is resolved now.
  // First from the heap, where the staged walk (16 KB ring) mostly fits with the buffer released;
  // what is still short of memory then comes from the framebuffer itself: reclaim the freed hole
  // as the one 52 KB block (a plain realloc), borrow it for the walk, and free it again so the
  // restore below runs exactly as before. That is the region an X3 heap can never offer
  // otherwise -- after a fragmented build not even released (2026-09-25: 28,660 contig against a
  // 33,280 walk left a chapter cached with none of its 27 images). Only a resolve earns the one
  // rebuild -- the manifest then answers with no ring at all -- so a walk that finds nothing
  // leaves the build as it is.
  // The released build is the path with the most heap this session can offer; a result it
  // still had to degrade is worth one loud line (the status byte carries it to the next entry,
  // which rebuilds once -- see the cache-hit policy in buildSection).
  if (createOk && (section->isCssLowHeapDegraded() || section->isTableRowDegraded())) {
    LOG_ERR("ERS", "Blocking build spine=%d came out degraded (%s%s); cached with the status bit set",
            currentSpineIndex, section->isCssLowHeapDegraded() ? "css-skips " : "",
            section->isTableRowDegraded() ? "table-row-demoted" : "");
  }
  if (createOk && section->isImageHeaderDegraded()) {
    // A borrowed build's region is idle now (its build state is gone): walk from it directly,
    // as Background-C does at its end. Nothing below can offer more room than that.
    if (borrowedHere) buildScratch_->reset();
    bool imagesResolved = epub->persistImageManifest(borrowedHere ? buildScratch_.get() : nullptr);
    const EpubImageManifest* manifest = epub->getImageManifest();
    if (manifest && manifest->hasPending() && !borrowedHere) {
      if (released) {
        if (renderer.reallocSecondaryBuffer()) {
          imagesResolved = resolvePendingImageHeadersFromFramebuffer() || imagesResolved;
          renderer.releaseSecondaryBuffer();
        } else {
          LOG_ERR("ERS", "Section %d: no block for the image header walk; images stay deferred", currentSpineIndex);
        }
      } else {
        imagesResolved = resolvePendingImageHeadersFromFramebuffer() || imagesResolved;
      }
    }
    if (imagesResolved) {
      LOG_INF("ERS", "Section %d: image headers resolved after the build; rebuilding once", currentSpineIndex);
      section->clearCache();
      const uint32_t rebuildStart = millis();
      createOk = runCreate();
      LOG_INF("ERS", "createSectionFile image rebuild returned %d in %ums (free=%lu)", createOk ? 1 : 0,
              millis() - rebuildStart, esp_get_free_heap_size());
      checkHeapIntegrity("after_createSectionFile_image_rebuild");
    }
  }

  // No eager image pre-decode here. Only the dimensions are needed to lay a section out, and
  // those come from the ZIP entry header at parse time (ImageDecoderFactory::getDimensions-
  // FromZipEntry) — nothing about building the section requires an image to be extracted.
  //
  // This pass used to decode EVERY image in the section while the framebuffer was released, on
  // the theory that this is the point of maximum contiguous heap and renderContents' own warm
  // pass would have less headroom. That premise is stale: the per-page warm now BORROWS the
  // secondary framebuffer as a decode arena rather than competing for contiguous heap
  // (docs/memory-allocation-strategy.md §9.3, device-measured on both panels), and it applies
  // exactly the same force-load / monochrome / grayscale policy this pass did.
  //
  // What it cost was time to first page: measured on X4, 72 s before anything appeared — 11
  // pages of images, each decoded twice (1-bit plane plus the AA grayscale plane) at ~0.6-3.2 s
  // per decode, whether or not the reader ever turned to those pages. Per-image cost is
  // unchanged and still amortised by the .pxc; it is now paid by the page that needs it.
  // In-place builds have always behaved this way.

  // Restore the secondary buffer only if we released it. The realloc gives a white baseline that
  // no longer matches the panel, so arm a one-shot half-refresh (X4 only). The in-place path
  // leaves the buffer — and the baseline — untouched, so the first page uses a normal fast
  // refresh.
  const BuildOutcome outcome = createOk ? BuildOutcome::Built : BuildOutcome::Failed;
  if (borrowedHere) {
    handBackBorrow();
    // Symmetric with the released path below: a prior IncrementalReleased build may have left
    // single-buffer fast-diff opted in; clear it now that the double buffer is back. No RED-RAM
    // sync here either -- the return re-seeded the baseline exactly as a realloc does.
    renderer.setSingleBufferFastDiff(false);
    LOG_INF("ERS", "Index end mem (after fb return): free=%lu contig=%lu", esp_get_free_heap_size(),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  }
  if (released) {
    if (!reallocSecondaryEvictingCaches()) {
      LOG_ERR("ERS", "Failed to reallocate secondary display buffer — display quality degraded");
      secondaryBufferDegraded_ = true;
      const uint32_t freeAfterIndex = esp_get_free_heap_size();
      // Do NOT call heap_caps_get_largest_free_block here: the heap may be corrupt after image
      // decode failures under pressure, and walking the TLSF free-block list on a corrupt heap
      // causes an interrupt WDT crash. Pass 0 for contigHeap — the restart heuristic treats 0 as
      // "contiguous block definitely too small", which is correct: malloc for ~52 KB just failed.
      LOG_ERR("ERS", "Heap after index: free=%lu", freeAfterIndex);
      // Same one-shot forensics as the AA-path failure: run 13 lost the buffer here (53 236
      // free after the release, 34 804 / 40 948 largest after the build) with nothing to say
      // what the build had left in the hole.
      {
        static bool dumpedHeapOnce = false;
        if (!dumpedHeapOnce) {
          dumpedHeapOnce = true;
          LOG_ERR("ERS", "Heap pin forensics (one-shot, free spans >= 1 KB and their neighbours):");
          logHeapPinForensics();
        }
      }
      if (maybeRestartForFragmentedHeap(freeAfterIndex, 0)) {
        return BuildOutcome::Restarting;
      }
    } else {
      secondaryBufferDegraded_ = false;
      // Symmetric with recoverSecondaryBufferIfNeeded(): a prior IncrementalReleased build may have
      // left single-buffer fast-diff opted in, and a failed opportunistic realloc could route the
      // re-render here instead. Clear it now that the double buffer is back so the next FAST refresh
      // uses the normal host-reseeded baseline, not a stale controller-retained one. No-op if it was
      // never set.
      renderer.setSingleBufferFastDiff(false);
      // Do NOT syncRedRamFromFrameBuffer() here: reallocSecondaryBuffer() whitened the new secondary,
      // and syncRedRamFromFrameBuffer() would copy that white into RED RAM, destroying the baseline.
      // RED already holds the frame displayed before the build (the popup); reseeding from white made
      // the first page after the build diff against white and ghost.
      LOG_DBG("ERS", "Index end mem (after fb realloc): free=%lu", esp_get_free_heap_size());
    }
  }
  checkHeapIntegrity("after_fb_realloc");
  return outcome;
}

bool EpubReaderActivity::resolvePendingImageHeadersFromFramebuffer() {
  EpubImageManifest* manifest = epub ? epub->getImageManifest() : nullptr;
  if (!manifest || !manifest->hasPending() || secondaryBorrowed_ || !renderer.hasSecondaryBuffer()) return false;
  size_t size = 0;
  uint8_t* borrowed = renderer.borrowSecondaryBuffer(&size);
  if (!borrowed) return false;
  bool resolved = false;
  {
    // Scoped so the arena is gone before the buffer goes back (see the warm pass for the
    // use-after-free this ordering prevents). The walk's ring is a scoped block inside it.
    BuildArena arena(borrowed, size);
    if (arena.valid()) {
      resolved = epub->persistImageManifest(&arena);
      LOG_INF("ERS", "Image header walk from the framebuffer: resolved=%d, still pending=%d", resolved ? 1 : 0,
              manifest->hasPending() ? 1 : 0);
    }
  }
  renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  return resolved;
}

bool EpubReaderActivity::buildSection(const RenderLayout& layout) {
  const int spineCount = epub->getSpineItemsCount();
  if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) {
    LOG_ERR("ERS", "Render rejected invalid spine index %d (valid 0..%d)", currentSpineIndex, spineCount - 1);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
    automaticPageTurnActive = false;
    requestUpdate();
    return false;
  }

  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;
  const bool embeddedStyle = lastRenderStats.embeddedStyle;
  const uint8_t imageRendering = lastRenderStats.imageRendering;
  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  // Adopt the Background-B Section when entering exactly the spine it was working on: a
  // completed background build turns into a plain cache hit below, and a partial build
  // resumes in the indexing path instead of restarting from scratch. On any other
  // navigation the B state is stale — drop it (aborting a partial build). While a build
  // is live, loadSectionFile must be skipped: it would clobber the live write handle.
  bool resumeBackgroundBuild = false;
  if (backgroundSection_ && backgroundBuildSpineIndex_ == currentSpineIndex) {
    resumeBackgroundBuild = backgroundSection_->hasActiveBuild();
    // Distinguish the three adopt cases for an accurate log: a live partial build (resume), a
    // B-completed section with pages (cache hit follows), or a section B only probed and parked
    // in WaitHeap (no pages — loadSectionFile will miss and a foreground/C build follows).
    const char* adoptKind = resumeBackgroundBuild                 ? "resuming partial build"
                            : (backgroundSection_->pageCount > 0) ? "build complete"
                                                                  : "probed only, will build";
    section = std::move(backgroundSection_);
    // Transfer, don't return: if B was building inside the borrowed secondary framebuffer, the
    // adopted build is still live inside that arena and Background-C wants exactly the same
    // borrow for exactly the same reason. Clearing the B-owns-it flag (leaving secondaryBorrowed_
    // and buildScratch_ intact, now reachable through `section`) hands ownership to C, whose
    // existing return sites — recoverSecondaryBufferIfNeeded, the C fallback paths, onExit —
    // take it from here. Must happen BEFORE resetBackgroundBuild(), which would otherwise call
    // endBackgroundBorrow() and yank the arena out from under the build we just adopted.
    if (backgroundBorrowActive_) {
      backgroundBorrowActive_ = false;
      backgroundBuildThrough_ = false;
      // B's part of the build ends here: read its heap low now, or the monitor stays on for good
      // (endBackgroundBorrow() never runs for a transferred borrow).
      uint32_t lowWhileLent = 0;
      if (backgroundHeapLowTracked_) {
        lowWhileLent = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
        heap_caps_monitor_local_minimum_free_size_stop();
        backgroundHeapLowTracked_ = false;
      }
      LOG_INF("ERS", "Background-B borrow transferred to Background-C for spine %d (heap %lu at start, %lu lowest)",
              currentSpineIndex, static_cast<unsigned long>(backgroundBorrowFreeAtStart_),
              static_cast<unsigned long>(lowWhileLent));
    }
    LOG_INF("ERS", "Adopting background section for spine %d (%s)", currentSpineIndex, adoptKind);
  } else {
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
  }
  chapterSpanSpine_ = -1;  // new spine or new settings: the siblings' caches must be looked up again
  resetBackgroundBuild();
  const unsigned long sectionStart = millis();

  // The gather is NOT run here any more. It is a whole-book two-pass scan (measured 2822 ms on
  // X3 for alice-illustrated, which has no footnotes at all) and running it before every first
  // section build charged every reader open for it, whether or not the book has notes. It now
  // runs on the first page that actually contains a footnote -- see render(). Sections built
  // before that are cached honestly as previews-off.

  // A resumed partial Background-B build has no on-disk LUT yet, so skip loadSectionFile (it
  // would clobber the live write handle); it always needs building. Otherwise probe the cache.
  Section::BuildParams probeParams = makeSectionBuildParams();
  probeParams.viewportWidth = viewportWidth;
  probeParams.viewportHeight = viewportHeight;
  probeParams.embeddedStyle = embeddedStyle;
  probeParams.imageRendering = imageRendering;
  bool cacheHit = !resumeBackgroundBuild && section->loadSectionFile(probeParams);

  // A cache written by an interrupted build is marked truncated (parseComplete was
  // false when it was persisted). With at least one page that is still readable and
  // the mitigation hint tells the user. With ZERO pages it is unusable AND
  // unrecoverable: loadSectionFile() returns true, so this counts as a hit, nothing
  // rebuilds it, and the chapter renders "No pages to render" on every open for as
  // long as the file exists. Seen on the T5S3, where Background-B builds are
  // preempted often enough to leave one behind:
  //   Deserialization succeeded: 0 pages
  //   Cache found, skipping build...
  //   Section 6 is truncated; showing mitigation hint
  //   No pages to render
  // Both in-session build paths already discard a truncated result and rebuild
  // (see fallbackToReleasedRebuild and the Background-B branch); only a cache read
  // from disk on a cold open lacked the same treatment. Drop the file so the miss
  // is permanent rather than repeated on the next open.
  if (cacheHit && section->isTruncatedCache() && section->pageCount == 0) {
    LOG_INF("ERS", "Section %d: cached file is truncated with 0 pages; discarding and rebuilding", currentSpineIndex);
    section->clearCache();
    cacheHit = false;
  }

  // A cache whose build dropped images because the heap could not size them right then (the
  // header walk needs ~33 KB contiguous; X3 2026-09-25: a rebuild on a fragmented heap left all 27
  // images of a chapter out, and the heap-recovery reboot that followed would have had room for
  // every one). The rebuild resolves them while the buffer is released. Once per spine per
  // session: if even that build comes out degraded, reading on beats rebuilding on every entry.
  if (cacheHit && section->isImageHeaderDegraded() && imageHeaderRebuildSpine_ != currentSpineIndex) {
    LOG_INF("ERS", "Section %d: cached without images the heap could not size; rebuilding", currentSpineIndex);
    imageHeaderRebuildSpine_ = currentSpineIndex;
    section->clearCache();
    cacheHit = false;
  }
  // A cache whose layout is what the heap allowed rather than what the book says: a table row
  // written as paragraphs, or elements cached without their styles because the resolver skipped
  // lookups. Both used to be baked in for good (memory audit 2026-09, F3). Same policy as the
  // image bit: one rebuild per spine per session, and if that one comes out degraded too the
  // bit stays set and the chapter is read as it is.
  if (cacheHit && (section->isTableRowDegraded() || section->isCssLowHeapDegraded()) &&
      degradedLayoutRebuildSpine_ != currentSpineIndex) {
    LOG_INF("ERS", "Section %d: cached %s under low heap; rebuilding once", currentSpineIndex,
            section->isTableRowDegraded() ? "with a table row demoted to paragraphs" : "with CSS lookups skipped");
    degradedLayoutRebuildSpine_ = currentSpineIndex;
    section->clearCache();
    cacheHit = false;
  }

  // Deterministic: a fixed-capacity limit (footnotes per page, anchors per chapter, nesting
  // depth, ...) changed what this chapter shows. Nothing to rebuild; worth a line in the log.
  if (cacheHit && section->isSimplified()) {
    LOG_INF("ERS", "Section %d: cached simplified (a fixed-capacity limit was exceeded at build time)",
            currentSpineIndex);
  }

  const bool cssFallbackRebuild = cacheHit && section->isEmbeddedStyleFallback();
  const bool needBuild = resumeBackgroundBuild || !cacheHit || cssFallbackRebuild;
  // The decisive fact for wake-latency work: a probe that hits means the section cost is a
  // LUT read, a miss means a full rebuild sits between the wake and the page. Recorded as
  // "was a build needed", not the raw probe result, so a CSS-fallback rebuild counts as a miss.
  WakeTrace::setSectionCacheHit(!needBuild);

  // One grep-able line per section entry tying the cache decision to the effective
  // preview inputs — discriminates "stale variant loaded" from "effective value not
  // what the UI shows" from "parser gate leak" in field logs.
  // `previews` is the SETTING; `variant` is what actually keys the cache — they differ before the
  // deferred gather has run, which is precisely the state this line exists to make visible.
  LOG_INF("ERS", "Section probe spine=%d: cacheHit=%d previews=%d variant=%d (override=%d global=%u)",
          currentSpineIndex, cacheHit ? 1 : 0, getEffectiveInlineFootnotePreviews() ? 1 : 0,
          probeParams.inlineFootnotePreviews ? 1 : 0, static_cast<int>(bookInlineFootnotePreviewsOverride),
          static_cast<unsigned>(SETTINGS.inlineFootnotePreviews));

  if (needBuild) {
    lastRenderStats.cacheRebuilt = true;

    // Background-C: build the current section incrementally on the loop task so input stays
    // responsive and pages appear as they are written. Used for the clean cases — a cache miss
    // or a resumed partial B build. The CSS-fallback rebuild keeps the blocking path: the section
    // already shows usable fallback content.
    // The spine's uncompressed size sizes the extraction inflate ring, so the resident/released
    // choice needs it (one central-dir scan; ~zero next to the ~1.4 s a doomed resident attempt
    // wastes). A lookup failure leaves 0 = unknown -> static floors only, the old behaviour.
    size_t inflatedSize = 0;
    epub->getSpineItemInflatedSize(currentSpineIndex, &inflatedSize);
    SectionBuildMode mode = (resumeBackgroundBuild || !cacheHit) && !cssFallbackRebuild
                                ? chooseSectionBuildMode(embeddedStyle, inflatedSize)
                                : SectionBuildMode::Blocking;
#ifdef EHP_FORCE_BLOCKING_BUILD
    // Experiment harness only (see docs/memory-allocation-strategy.md 8.4a). Pins every section to
    // the blocking path so two instrumented runs are comparable: the Background-C decision depends
    // on live heap, which is the very thing under measurement, and a run that takes a different
    // path -- or aborts before reaching the construct being measured -- produces numbers that
    // cannot be read against the other arm. Never defined in a shipped build.
    mode = SectionBuildMode::Blocking;
#endif
    const bool incremental = mode != SectionBuildMode::Blocking;
    bool runBlocking = !incremental;

    // Single grep-able marker for the build mode actually taken for this spine — pairs with the
    // heapAllowsInPlaceBuild line above to explain every section-entry build decision from the log.
    LOG_INF("ERS", "Section build mode spine=%d: %s (cacheHit=%d cssFallback=%d resumeB=%d)", currentSpineIndex,
            mode == SectionBuildMode::Blocking              ? "BLOCKING"
            : mode == SectionBuildMode::IncrementalResident ? "INCR_RESIDENT"
                                                            : "INCR_RELEASED",
            cacheHit ? 1 : 0, cssFallbackRebuild ? 1 : 0, resumeBackgroundBuild ? 1 : 0);

    if (incremental) {
      // Capture the "dramatic transition" signal before anything can display, because a popup's
      // refresh would otherwise consume the pending exit-full-refresh override:
      //   - cold open of this book (coldOpenHalfRefreshArmed_), or
      //   - a pending exit-full-refresh override left by a deliberate jump (chapter/percent/footnote)
      //     to a possibly-uncached section. Capture it now (a peek, not a consume) so the content
      //     page below can be forced to HALF; without capturing it the content page would fall back
      //     to a FAST diff against the popup frame and ghost its outline.
      const bool dramaticTransition = coldOpenHalfRefreshArmed_ || renderer.hasRefreshOverridePending();
      coldOpenHalfRefreshArmed_ = false;

      // Whether the popup has to be painted before the build starts, or can wait until the first
      // slice proves the build is actually long enough to need one.
      //
      // A RELEASED build has no choice: drawPopup() composites via syncWriteBufferFromDisplayed(),
      // which reads frameBufferActive, and the release below takes that copy away — and the
      // released path then seeds RED RAM *from the popup frame* so its mid-build FAST refreshes
      // have a baseline to diff against. Both depend on the popup already being on screen.
      //
      // A RESIDENT build keeps the buffer, so neither applies and the popup can be deferred. That
      // matters because the kickoff slice below frequently finishes the whole section: a one-page
      // cover measured 309 ms start-to-finish on X4, and every millisecond of the popup it painted
      // first (a ~540 ms FAST refresh) was spent on a frame the very next render replaced. Worse,
      // the popup is what makes the replacing page expensive — it is a dark box the content page
      // must then diff against, which is the entire reason for the ~1.7 s HALF armed below.
      const bool popupBeforeBuild = (mode == SectionBuildMode::IncrementalReleased);
      if (popupBeforeBuild) {
        // X4: the dramatic-transition HALF belongs on the first CONTENT page
        // (forceHalfRefreshAfterPopup_ below), not the popup. The popup is a transient box over the
        // already-correct current page, so a FAST overlay is clean and instant — drop the pending
        // HALF override here so drawPopup() doesn't spend a second ~1.7s HALF on the popup itself.
        // On X3 the popup's own refresh IS the baseline step (its displayBuffer updates DTM1, which
        // the following FAST content page diffs against), so leave the override for drawPopup().
        if (renderer.fastRefreshNeedsHostFrame() && dramaticTransition) {
          renderer.clearRefreshOverride();  // discard the armed HALF -> popup paints FAST
        }
        GUI.drawPopup(renderer, tr(STR_INDEXING));  // immediate feedback before the first page lands

        // Tight heap: free the secondary buffer (~48–52 KB) for the build. AA is off until the
        // build ends and recoverSecondaryBufferIfNeeded() reallocates it (marked via
        // secondaryBufferDegraded_); mid-build draws are BW. No display downside on X3 (baseline
        // in controller).
        //
        // X4: a long chapter can keep this build running for many page turns (10s of seconds),
        // and displayBuildPage() requests FAST refreshes via the normal cadence the whole time —
        // without the opt-in below, EInkDisplay::triggerDisplay() silently downgrades every one
        // of those to HALF (no host-side previous-frame copy to diff against), so every mid-build
        // page turn pays the slow waveform. Seed RED RAM from the popup frame now on screen (drawn
        // just above, while the buffer was still resident) BEFORE releasing, then opt in to
        // single-buffer fast differential so FAST refreshes keep diffing against the controller's
        // retained RED RAM copy. Symmetric setSingleBufferFastDiff(false) lives in
        // recoverSecondaryBufferIfNeeded(), the one place this released state gets cleanly restored.
        const uint32_t freeBefore = esp_get_free_heap_size();
        // Already holding the borrow (transferred from Background-B with its build live): the
        // block is lent, buildScratch_ points at it and `section` is already wired to that arena.
        // Re-running the sequence below would re-seed RED RAM from a frame that is no longer the
        // one on screen, and — since borrowSecondaryBuffer() returns null when already lent —
        // fall into releaseSecondaryBuffer() on a buffer the display no longer owns. Just keep
        // the single-buffer display flags asserted and let the adopted build carry on.
        if (secondaryBorrowed_) {
          // Still seed RED RAM and opt in to single-buffer fast differential: unlike B (which
          // never refreshes while borrowing) C drives FAST refreshes for every mid-build page.
          // drawPopup() above just painted frameBuffer with what is now on screen, so this is
          // the correct baseline — the same frame the non-transferred branch below syncs from.
          renderer.syncRedRamFromFrameBuffer();
          renderer.setSingleBufferFastDiff(true);
          secondaryBufferDegraded_ = true;
          LOG_INF("ERS", "Background-C: building spine %d incrementally, secondary buffer already BORROWED (free=%lu)",
                  currentSpineIndex, static_cast<unsigned long>(freeBefore));
        } else {
          renderer.syncRedRamFromFrameBuffer();
          // Prefer BORROWING the secondary buffer over freeing it: the lent block never enters
          // the heap, so a survivor can't split it into a fragmented hole and the return cannot
          // fail (the realloc-failure / heap-recovery-restart class of bugs is impossible). The
          // build's arena bump-allocates inside the lent region instead of the heap. Fall back to
          // the legacy release only when there is no secondary buffer to lend.
          size_t borrowedSize = 0;
          uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize);
          if (borrowed) {
            buildScratch_ = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
            section->setExternalBuildScratch(buildScratch_ && buildScratch_->valid() ? buildScratch_.get() : nullptr);
            secondaryBorrowed_ = true;
          } else {
            renderer.releaseSecondaryBuffer();
          }
          // Display semantics are identical to a released buffer (single-buffer mode either way).
          renderer.setSingleBufferFastDiff(true);
          secondaryBufferDegraded_ = true;
          LOG_INF("ERS", "Background-C: building spine %d incrementally, secondary buffer %s (free %lu->%lu)",
                  currentSpineIndex, borrowed ? "BORROWED" : "RELEASED", freeBefore, esp_get_free_heap_size());
        }
      } else {
        LOG_INF("ERS", "Background-C: building spine %d incrementally, buffer resident (free=%lu contig=%lu)",
                currentSpineIndex, esp_get_free_heap_size(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
      }
      // Force the first REAL page that replaces the popup — whether shown by displayBuildPage()
      // (multi-slice build) or directly by renderContents() (build finishes in one slice, e.g. a
      // one-page cover) — to HALF, so a dramatic content change (text popup -> photo) doesn't leave
      // a ghost outline of the popup box, and so released-buffer builds don't compound that ghosting
      // across many subsequent FAST mid-build pages before the periodic full-resync cadence cleans
      // it up. X4 only: its FAST refresh needs a host-side previous-frame copy (or the single-buffer-
      // fast-diff opt-in above) to diff against, so a dramatic frame change can under-drive pixels.
      // X3's fast differential reads the controller's own DTM1 RAM, which drawPopup()'s displayBuffer()
      // call already updated correctly — no host-side baseline gap to paper over, so forcing HALF
      // there is pure unnecessary cost. A routine forward-reading crossing into a still-building
      // Background-B section is NOT dramatic (neither signal is set), so it keeps the fast cadence.
      if (renderer.fastRefreshNeedsHostFrame() && dramaticTransition && popupBeforeBuild) {
        forceHalfRefreshAfterPopup_ = true;
      }
      renderer.clearFontAccumulation();
      readerPhase_ = ReaderPhase::PRECOMPILING;
      // Seed the display cursor so the SectionBuilding pass knows which page to show first. For a
      // Page target it's the requested page (usually 0); other targets show the popup until the
      // build completes and the position is resolved, so the exact value doesn't matter there.
      section->currentPage = (navTarget.kind == NavigationTarget::Kind::Page) ? navTarget.page : 0;
      buildDisplayedPage_ = -1;
      buildingPopupShown_ = popupBeforeBuild;
      // Kick the build off so hasActiveBuild() is true (a resumed B build already is). One slice
      // may already finish a tiny section, in which case we fall through to a normal render.
      Section::BuildStep step = Section::BuildStep::More;
      if (!section->hasActiveBuild()) {
        step = section->stepSectionBuild(makeSectionBuildParams(), BG_BUILD_BUDGET_MS);
      }
      if (step == Section::BuildStep::More) {
        // The deferred case, now resolved: the build needs more slices, so a popup IS warranted.
        // renderSectionBuildingPass() paints it on the next pass (its `if (!buildingPopupShown_)`
        // branch), which is also where the reader would otherwise sit with no feedback. Arm the
        // same transition handling the up-front path arms, so a popup drawn late is followed by
        // the same clean content refresh as one drawn early.
        if (!popupBeforeBuild && renderer.fastRefreshNeedsHostFrame() && dramaticTransition) {
          renderer.clearRefreshOverride();
          forceHalfRefreshAfterPopup_ = true;
        }
        requestUpdate();  // SectionBuilding pass + stepCurrentSectionBuild() take over
        return false;
      }
      // Fell through: the kickoff finished the section. On the deferred path no popup was ever
      // painted, so there is no box for the content page to ghost against and no armed HALF --
      // the page lands on the normal refresh cadence.
      readerPhase_ = ReaderPhase::READING;
      if (step == Section::BuildStep::Failed) {
        LOG_ERR("ERS", "Background-C kickoff failed for spine %d; using blocking path", currentSpineIndex);
        // The failed kickoff tore its build state down (releasing into the arena). If we borrowed
        // the secondary buffer above, hand it back before the blocking path: compileSectionCache
        // manages a resident buffer (releases it for real heap), so it must start with the block
        // back on the display. The return cannot fail — the region never entered the heap.
        if (secondaryBorrowed_) {
          section->setExternalBuildScratch(nullptr);
          buildScratch_.reset();
          renderer.returnSecondaryBuffer();
          secondaryBorrowed_ = false;
          secondaryBufferDegraded_ = false;
          renderer.setSingleBufferFastDiff(false);
        }
        forceBlockingBuildSpine_ = currentSpineIndex;
        runBlocking = true;
      }
      // step == Done: tiny section finished in one slice — fall through to resolveInto/return true.
    }

    if (runBlocking) {
      if (!incremental) {
        // Background-C was not chosen (CSS-fallback rebuild, no secondary buffer, or the
        // blocking-fallback latch is set for this spine) — log so the mode is visible in traces.
        LOG_INF("ERS", "Background-C declined for spine %d (cssFallback=%d hasBuf=%d latched=%d); blocking build",
                currentSpineIndex, cssFallbackRebuild ? 1 : 0, renderer.hasSecondaryBuffer() ? 1 : 0,
                forceBlockingBuildSpine_ == currentSpineIndex ? 1 : 0);
      }
      const BuildOutcome outcome = compileSectionCache(layout, embeddedStyle, imageRendering);
      if (outcome == BuildOutcome::Restarting) {
        return false;  // fragmented-heap recovery reboot in progress
      }
      renderer.restoreFontMetadata();
      readerPhase_ = ReaderPhase::READING;
      if (outcome == BuildOutcome::Failed) {
        if (cssFallbackRebuild) {
          LOG_ERR("ERS", "Failed to rebuild CSS section cache; keeping fallback");
          section->loadSectionFile(probeParams);
        } else {
          LOG_ERR("ERS", "Failed to build section; showing empty chapter");
          // Do NOT reset section: leave it alive with pageCount=0 so getRenderPass()
          // returns Normal on the next cycle (not BuildSection), breaking the retry loop.
          // renderNormalPass handles pageCount==0 gracefully with an "empty chapter" screen.
          requestUpdate();
          return false;
        }
      }
    }
  } else {
    LOG_DBG("ERS", "Cache found, skipping build...");
  }
  // Any section entry that resolved here without an ongoing incremental build (cache hit, blocking
  // build, or a tiny incremental build that finished in one slice) spends the cold-open arm: it is
  // valid for the FIRST section entry only, so a cached re-open's later forward crossings into
  // still-building sections are not mistaken for the dramatic cold-open transition. A multi-slice
  // incremental build returns earlier and has already consumed the arm in its popup branch.
  coldOpenHalfRefreshArmed_ = false;
  lastRenderStats.sectionLoadMs = millis() - sectionStart;

  if (section->isTruncatedCache() && currentSpineIndex != lastWarnedTruncatedSpineIndex) {
    lastWarnedTruncatedSpineIndex = currentSpineIndex;
    truncatedSectionHintRendersRemaining = TRUNCATED_SECTION_HINT_RENDER_COUNT;
    LOG_INF("ERS", "Section %d is truncated; showing mitigation hint", currentSpineIndex);
  }

  // Section is ready (cache hit, blocking build, or a tiny C build that finished in one slice):
  // the Background-C fallback latches for this spine have served their purpose.
  if (needBuild) SerialTestApi::noteBuildDone(currentSpineIndex, section->pageCount);
  forceBlockingBuildSpine_ = -1;
  forceReleasedBuildSpine_ = -1;

  LOG_DBG("ERS", "resolveInto: navTarget.kind=%d pageCount=%d", (int)navTarget.kind, (int)section->pageCount);
  navTarget.resolveInto(*section, currentSpineIndex);
  LOG_DBG("ERS", "resolveInto result: currentPage=%d", (int)section->currentPage);
  settleFollowedLink(section->currentPage);
  anchorNavTargetToCurrentPage();
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  return true;
}

void EpubReaderActivity::renderNormalPass(RenderLock& lock, const RenderLayout& layout) {
  const int orientedMarginTop = layout.marginTop;
  const int orientedMarginRight = layout.marginRight;
  const int orientedMarginBottom = layout.marginBottom;
  const int orientedMarginLeft = layout.marginLeft;

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    const unsigned long pageLoadStart = millis();
    auto p = section->loadPageFromSectionFile();
    lastRenderStats.pageLoadMs = millis() - pageLoadStart;
    if (!p) {
      // Escalate in two steps instead of assuming the cache is damaged. The dominant reason
      // this returns null is Page::deserialize failing to allocate under pressure — the file
      // on disk is fine, and clearing it charges a full chapter re-index for one failed malloc.
      // See the pageLoadFail* latch for the state machine.
      if (pageLoadFailSpine_ != currentSpineIndex || pageLoadFailPage_ != section->currentPage) {
        pageLoadFailSpine_ = currentSpineIndex;
        pageLoadFailPage_ = section->currentPage;
        pageLoadFailStage_ = 1;
      } else if (pageLoadFailStage_ < 2) {
        pageLoadFailStage_ = 2;
      } else {
        pageLoadFailStage_ = 3;
      }
      automaticPageTurnActive = false;
      if (pageLoadFailStage_ == 1) {
        // Transient hypothesis: hand back everything the reader is holding that nobody is
        // waiting on, then reload the section from the SAME cache file. The evictions are the
        // ones reallocSecondaryEvictingCaches() already trusts for the harder case (a 48 KB
        // contiguous request): font page slots are rebuilt by the next prewarm regardless, the
        // CSS caches reload lazily from SD, and Background-B's lookahead section is pure
        // speculation that re-probes on its own cadence. navTarget carries the position, so the
        // re-entry costs a LUT re-read, not a rebuild.
        LOG_ERR("ERS", "Page %d load failed (spine %d, free=%lu); evicting caches and reloading section",
                section->currentPage, currentSpineIndex, static_cast<unsigned long>(esp_get_free_heap_size()));
        if (FontCacheManager* fontCache = renderer.getFontCacheManager()) {
          fontCache->clearCache();
        }
        if (epub && epub->getCssParser()) {
          epub->getCssParser()->clearCaches(/*evictEverything=*/true);
        }
        resetBackgroundBuild();
        section.reset();
        requestUpdate();
        return;
      }
      if (pageLoadFailStage_ == 2) {
        // Failed again at the same page after a cache eviction and a fresh LUT read. That does
        // not prove the file is damaged — the heap may simply still be short — but the transient
        // hypothesis has had its chance, so fall through to what this site always did. The free
        // figure in both log lines is what tells the two apart after the fact.
        LOG_ERR("ERS", "Page %d load failed again (spine %d, free=%lu); clearing section cache and rebuilding",
                section->currentPage, currentSpineIndex, static_cast<unsigned long>(esp_get_free_heap_size()));
        section->clearCache();
        section.reset();
        requestUpdate();
        return;
      }
      // Stage 3: a freshly rebuilt cache cannot serve this page either. Re-entering would loop
      // forever (the old TODO at this call site), so stop and show the empty-chapter screen —
      // the reader can still navigate out with the section left intact.
      LOG_ERR("ERS", "Page %d unreadable after rebuild (spine %d); giving up on this page", section->currentPage,
              currentSpineIndex);
      TapTargets::readerLinks().invalidate();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      return;
    }
    // Loaded: retire any escalation recorded against this page.
    if (pageLoadFailStage_ != 0) {
      pageLoadFailSpine_ = -1;
      pageLoadFailPage_ = -1;
      pageLoadFailStage_ = 0;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    lastRenderStats.hadImages = p->hasImages();
    lastRenderStats.footnoteCount = static_cast<int>(currentPageFootnotes.size());
    lastRenderStats.spineIndex = currentSpineIndex;
    lastRenderStats.pageIndex = section->currentPage;
    lastRenderStats.pageCount = section->pageCount;
    showTruncatedSectionHintThisRender = truncatedSectionHintRendersRemaining > 0;

    const auto start = millis();
    renderContents(lock, std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    lastRenderStats.requestRenderMs = millis() - start;
    // A page is now on screen, however it got there. Background-B's quiet period runs from here.
    lastPageOnScreenMs_ = millis();
    if (truncatedSectionHintRendersRemaining > 0) {
      truncatedSectionHintRendersRemaining--;
    }
    LOG_DBG("ERS", "Rendered page in %dms", lastRenderStats.requestRenderMs);
    checkHeapIntegrity("after_page_render");
  }
  // Re-acquire the render lock before any further state mutation.
  // renderContents() released it early (after triggerDisplay) to free the loop
  // task during the waveform. Everything below touches shared reader state and
  // must be serialised against loop()-side mutations.
  {
    RenderLock relock;

    // Defensive guard: section can be invalidated by loop-side flows while the
    // render lock was released during display waveform wait.
    if (!section) {
      LOG_ERR("ERS", "render: section became null after display completion");
      return;
    }

    pendingProgressSave.spineIndex = currentSpineIndex;
    pendingProgressSave.page = section->currentPage;
    pendingProgressSave.pageCount = section->pageCount;
    pendingProgressSave.pending.store(true, std::memory_order_release);
    lastRenderStats.freeHeapAfter = esp_get_free_heap_size();
    // Avoid heap walk in the hot render path; largest free block is sampled in index lifecycle logs.
    lastRenderStats.largestFreeBlockAfter = 0;
    lastRenderStats.valid = true;
    const uint32_t totalFontLookups = lastRenderStats.fontCacheHits + lastRenderStats.fontCacheMisses;
    const uint32_t fontHitRatePct =
        totalFontLookups > 0 ? (lastRenderStats.fontCacheHits * 100UL) / totalFontLookups : 0UL;
    LOG_DBG(
        "ERS",
        "Page summary: spine=%d page=%d/%d prerendered=0 refresh=%s mode=0x%02X renderMs=%lu "
        "prewarmMs=%lu bwMs=%lu displayMs=%lu fontHits=%lu fontMisses=%lu fontHitPct=%lu glyphCalls=%lu glyphUs=%lu",
        currentSpineIndex, lastRenderedPageIndex_, lastRenderedPageCount_, refreshModeName(lastPageRefreshMode_),
        lastPageDisplayModeByte_, lastRenderStats.requestRenderMs, lastRenderStats.phases.prewarmMs,
        lastRenderStats.phases.bwRenderMs, lastRenderStats.phases.displayMs, lastRenderStats.fontCacheHits,
        lastRenderStats.fontCacheMisses, fontHitRatePct, lastRenderStats.fontGetBitmapCalls,
        lastRenderStats.fontGetBitmapTimeUs);

    if (pendingScreenshot) {
      // The page has just been redrawn without the menu. Ask for the screenshot rather than take it
      // here: the refresh is still running and a pre-render may start during it, so the capture
      // waits until the panel holds this page and nothing uses the buffers (ScreenshotUtil::service).
      pendingScreenshot = false;
      ScreenshotUtil::request(ScreenshotUtil::Target::Card);
    }

    // Pre-render was already scheduled in renderContents() before the lock was
    // released, so the loop task could start it during the waveform wait.
  }
}

void EpubReaderActivity::renderSectionBuildingPass(RenderLock& lock, const RenderLayout& layout) {
  if (!section || !section->hasActiveBuild()) {
    // Build finished or was aborted between classifyRenderPass() and here — re-dispatch so the
    // correct pass (Normal, or a fresh BuildSection after a cross/fallback) runs.
    requestUpdate();
    return;
  }

  // Draw the requested page if Background-C has already written it. Only for an explicit Page
  // target (other targets resolve at completion). buildDisplayedPage_ records the target we've
  // already acted on so we neither redraw it every tick nor (via the C step's nudge gate) get
  // pinged every slice.
  const int target = section->currentPage;
  const int built = static_cast<int>(section->activeBuildPageCount());
  if (navTarget.kind == NavigationTarget::Kind::Page && target >= 0 && target < built) {
    if (target == buildDisplayedPage_) {
      return;  // already handled this target (drawn, or an image page we're waiting out)
    }
    // Text-only pages render cleanly without the image decode / secondary-buffer dance that the
    // lock-light build path avoids; an image target waits for the final Normal render but is
    // still marked handled so the C step stops nudging us about it.
    // The page's TextBlock bytes come from a block on the build's lent region, opened here and
    // closed by displayBuildPage before it releases the lock. ~7 KB of the ~10.5 KB a mid-build
    // draw used to put on the heap, at the one moment the X3's minimum free heap is set: the
    // draw lands next to the parse's own state (device run 8: 7,340 B). Declared before `page`
    // so the page dies first on every path.
    BuildArena* drawArena =
        (secondaryBorrowed_ && buildScratch_ && buildScratch_->valid()) ? buildScratch_.get() : nullptr;
    BuildArena::Block drawBlock;
    if (drawArena != nullptr) drawBlock = drawArena->reserveBlock();
    auto page = section->loadPageFromActiveBuild(static_cast<uint16_t>(target), drawArena);
    buildDisplayedPage_ = target;
    // Image pages are drawn too, their undecoded images as "indexing" placeholders (see
    // ImageBlock::PlaceholderOnlyScope in displayBuildPage). They used to be skipped -- and the
    // target marked handled -- so a page with an ornament sat behind the popup for the whole
    // build. The normal render redraws the page with its images once the build completes.
    if (page) {
      buildingPopupShown_ = false;
      // This page is now the one on screen, so it owns the footnote state too. Without this the
      // footnote list and the menu's "has footnotes" flag kept describing whatever page was
      // displayed BEFORE the build started — the reader can open both while a build runs.
      currentPageFootnotes = std::move(page->footnotes);
      displayBuildPage(lock, *page, layout, drawBlock.valid() ? &drawBlock : nullptr);
      return;  // the lock is already released; the page goes out of scope here
    }
    // Image page or load failure: fall through to the popup until the build completes. The
    // page (and its bytes in the block) go first, then the block.
    if (drawBlock.valid()) drawArena->release(drawBlock);
  }

  // Requested page not built yet (or it's an image page / non-Page target): show the indexing
  // popup once, leaving any already-displayed page underneath it.
  if (!buildingPopupShown_) {
    // The popup covers the page, so the links under it must stop answering taps -- following a
    // marker nobody can see would be the phantom-target bug with an extra step.
    TapTargets::readerLinks().invalidate();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    buildingPopupShown_ = true;
  }
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // Every render pass redraws the status bar, so a refresh held back for Background-B is paid by
  // this one (cleared before recoverSecondaryBufferIfNeeded, which may end B's borrow).
  statusRefreshDeferred_ = false;
  recoverSecondaryBufferIfNeeded();
  if (fontRelayoutPending_) {
    // The memory ladder changed the book's fonts (finishLadder): the chapter is laid out again with them.
    fontRelayoutPending_ = false;
    resetSectionKeepingPosition();
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    LOG_ERR("ERS", "EPUB has no spine items, aborting render");
    automaticPageTurnActive = false;
    return;
  }

  clampSpineIndex(spineCount);

  // The finished-book pass needs no layout/stats setup, and only arms a flag — the lock stays
  // held for the rest of this call and is released by the render task as usual.
  if (currentSpineIndex == spineCount) {
    renderFinishedBookPass(spineCount);
    return;
  }

  // Push the render-time guide-dots option before any page draws (the scheduled
  // pre-render also picks it up: it only runs after this). Unlike bionic reading
  // this is not part of the section cache key, so toggling needs no rebuild.
  TextBlock::setGuideDots(getEffectiveGuideDots());

  const RenderLayout layout = computeRenderLayout();
  lastRenderStats = {};
  lastRenderStats.orientation = static_cast<uint8_t>(renderer.getOrientation());
  lastRenderStats.marginTop = layout.marginTop;
  lastRenderStats.marginRight = layout.marginRight;
  lastRenderStats.marginBottom = layout.marginBottom;
  lastRenderStats.marginLeft = layout.marginLeft;
  lastRenderStats.viewportWidth = layout.viewportWidth;
  lastRenderStats.viewportHeight = layout.viewportHeight;
  lastRenderStats.embeddedStyle = getEffectiveEmbeddedStyle();
  lastRenderStats.imageRendering = getEffectiveImageRendering();
  lastRenderStats.effectiveFontId = getEffectiveReaderFontId();
  lastRenderStats.textAntiAliasing = getEffectiveTextAntiAliasing();
  lastRenderStats.freeHeapBefore = esp_get_free_heap_size();
  // Avoid heap walk in the hot render path; largest free block is sampled in index lifecycle logs.
  lastRenderStats.largestFreeBlockBefore = 0;
  showTruncatedSectionHintThisRender = false;

  // Hold off a pure pre-render while a deferred AA pass is still owed for the
  // CURRENT page. The pre-render writes the next page into frameBuffer, but the
  // deferred AA's cleanup (cleanupGrayscaleWithPreviousBuffer) ends by copying
  // frameBufferActive — the current page — back over frameBuffer. If the pre-render
  // ran first, that copy would clobber the pre-rendered next page, and the next
  // page turn (BufferDisplay) would then flush the stale current page with only a
  // fresh status bar drawn over it (observed on X3 as "every second page doesn't
  // change"). Keep pendingPreRender armed and bail; runDeferredGrayscalePass()
  // re-requests an update once the AA pass has run, at which point the pre-render
  // proceeds against the correct buffer state. Guard only the pre-render: a real
  // page turn (usePreRenderedBuffer / Normal) must still render immediately.
  //
  // Deferred-AA panels only. On the X3 the hazard is that it keeps _refreshPending
  // asserted for the whole multi-second waveform, which blocks
  // runDeferredGrayscalePass() (it self-gates on !isRefreshPending()) and lets the
  // pre-render slip in ahead of the AA. A panel running AA inline never needs this:
  // it clears _refreshPending in triggerDisplay(), so the deferred AA — which
  // serviceBackgroundWork() runs first — always completes before any pre-render, and
  // reordering its differential refresh / RED-RAM baseline only reintroduces ghosting.
  //
  // Generalised from `renderer.isX3()` after the T5S3 moved onto the deferred path:
  // without this guard its AA was preempted by the queued pre-render on EVERY page
  // ("Deferred AA ABORTED: planes=35ms gray=0ms"), so the anti-aliasing was computed
  // and thrown away every time. The guard belongs to deferring, not to the X3.
  // A navigation that skipped pageTurn()'s hand-off leaves the AA and pre-render of the page it
  // left: drop them first, so the guard below cannot shelve this pass as that page's pre-render.
  dropStagedWorkIfPositionMoved("render");
  if (usesDeferredAa() && pendingGrayscale_.active && pendingPreRender && !usePreRenderedBuffer &&
      classifyRenderPass() == RenderPass::PreRender) {
    // Logged so the pre-render chain has no silent link left: a deferred pass that is
    // held here and then never re-kicked is otherwise indistinguishable from one that was
    // never armed. Fires at most once or twice per page, on deferred-AA panels only.
    LOG_DBG("ERS", "PreRender deferred: AA owed");
    return;
  }

  // Classify the pass, then consume the pre-render flags.
  const RenderPass pass = classifyRenderPass();
  // First render() to get this far after a book open. Re-marking on later renders is harmless:
  // the summary is emitted at the first visible page and ignores everything after it.
  WakeTrace::mark(WakeTrace::Phase::RenderStart);
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  // Any other pass redraws the write framebuffer and flushes/swaps, destroying pre-rendered
  // pixels — so a completed pre-render must be discarded here even when its (spineIndex,
  // pageIndex) still matches the current page. Keeping ready=true across an intervening
  // Normal render (periodic status-bar/battery/clock update) let the next page turn take the
  // BufferDisplay path and flush the STALE current page with only a fresh status bar drawn
  // over it (observed as "page counter advances but content doesn't, every 30-40 pages" —
  // the battery-percent tick cadence). This does not cost the hit: renderContents() re-arms
  // pendingPreRender when ready is false, and the re-render runs during the same render's
  // waveform wait. The BufferDisplay/PreRender passes manage ready themselves.
  if (pass != RenderPass::PreRender && pass != RenderPass::BufferDisplay && preRenderedPage.ready) {
    preRenderedPage.ready = false;
    preRenderedPlanesStaged_ = false;
  }

  switch (pass) {
    case RenderPass::FinishedBook:
      // Handled above before layout setup; unreachable here.
      return;
    case RenderPass::PreRender:
      renderPreRenderPass(layout);
      return;
    case RenderPass::BufferDisplay:
      if (renderBufferDisplayPass(layout)) {
        return;
      }
      // Fast path could not be taken (load failed / image page) — fall through to Normal.
      renderNormalPass(lock, layout);
      return;
    case RenderPass::BuildSection:
      if (!buildSection(layout)) {
        return;
      }
      WakeTrace::mark(WakeTrace::Phase::SectionReady);
      // Flush any image dimensions this build just resolved (foreground path).
      epub->persistImageManifest();
      renderNormalPass(lock, layout);
      return;
    case RenderPass::SectionBuilding:
      renderSectionBuildingPass(lock, layout);
      return;
    case RenderPass::Normal:
      renderNormalPass(lock, layout);
      return;
  }
}

bool EpubReaderActivity::maybeRestartForFragmentedHeap(const uint32_t freeHeap, const uint32_t contigHeap) {
  // The block the realloc needs: one framebuffer (52,272 B on the X3, 48,000 B on the X4). A
  // constant 52 KB here used to sit 976 B under the X3's real size, so a heap with a block just
  // too small for the buffer could read as "not fragmented" and stay degraded.
  const uint32_t secondaryBufferBytes =
      static_cast<uint32_t>(renderer.getDisplayWidthBytes()) * static_cast<uint32_t>(renderer.getDisplayHeight());
  // Reboot-based defrag should only run when the failure clearly looks like fragmentation:
  // the buffer plus a reading session's working set is free, yet no block holds the buffer.
  // Stated relative to the buffer, not as a fixed 96 KB: the X3 sat at 96.3 KB free with a
  // 40.9 KB largest block for the rest of a session (device run 10) -- 1.8x the buffer free,
  // every refresh degraded, and the old absolute floor 2 KB out of reach.
  constexpr uint32_t RESTART_READING_MARGIN_BYTES = 32 * 1024;
  const uint32_t restartMinFreeHeap = secondaryBufferBytes + RESTART_READING_MARGIN_BYTES;

  if (fragmentationRecoveryRestartAttempted_) {
    return false;
  }
  if (freeHeap < restartMinFreeHeap || contigHeap >= secondaryBufferBytes) {
    return false;
  }

  fragmentationRecoveryRestartAttempted_ = true;
  saveStateForRestart();
  const int page = (section ? section->currentPage : 0);
  const int pageCount = (section ? section->pageCount : 0);

  // Release both framebuffers (primary + secondary already gone) to free ~48 KB
  // more contiguous heap, then do a pre-reboot warm pass for any images that
  // couldn't be decoded with only the secondary released. Pixel writes land in a
  // small scratch buffer (discarded on reboot); we only care about the .pxc files
  // that get written to SD so the next boot can render images without a decoder.
  if (section) {
    const size_t scratchSize = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
    auto* scratch = static_cast<uint8_t*>(malloc(scratchSize));
    if (scratch && renderer.releaseFrameBuffersWithScratch(scratch, scratchSize)) {
      LOG_ERR("ERS", "Pre-reboot image warm pass: freed primary fb, scratch=%u bytes", scratchSize);
      const bool preRebootForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
      // Every framebuffer is released here, so this is the one pass with room for the full
      // progressive workspace: a .pxc written at a coarser scale (or as the DC preview) because
      // the heap was short at the time is decoded again now, not replayed as it is.
      section->warmAllImageCaches(0, 0, preRebootForceLoad, /*monochromeOutput=*/true, /*alsoWarmGrayscale=*/false,
                                  /*redecodeCoarse=*/true);
      // scratch is leaked intentionally — reboot follows immediately
    } else {
      free(scratch);
    }
  }

  LOG_ERR("ERS", "Fragmented heap recovery: free=%lu contig=%lu, restarting directly to reader (spine=%d page=%d/%d)",
          freeHeap, contigHeap, currentSpineIndex, page, pageCount);
  if (trySilentRestartToReaderForHeapRecovery()) {
    return true;
  }
  LOG_ERR("ERS", "Heap recovery restart blocked by safety latch; staying in degraded mode");
  return false;
}

void EpubReaderActivity::saveStateForRestart() {
  // Persist the live position only when it is authoritative. During a section (re)build
  // (readerPhase_ != READING, or section already reset for one) the saved navTarget has not
  // been applied yet — a fresh Section still sits at currentPage 0 — and saving that would
  // clobber the user's real position in progress.bin, which already holds the correct value
  // (issue #75: progress reset to the chapter start after a heap-recovery reboot).
  if (section && readerPhase_ == ReaderPhase::READING) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }
  // The reboot skips onExit, which is where the stack is otherwise saved. Unlike the position it
  // does not depend on the build phase: every entry was recorded when its link was followed.
  saveLinkBackStack();
}

bool EpubReaderActivity::writeReaderProgressCache(const std::string& cachePath, const int spineIndex,
                                                  const int currentPage, const int pageCount, const uint8_t percent,
                                                  const std::optional<uint16_t> paragraphIndex,
                                                  const std::optional<EpubProgressRecord::Shown>& shown) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + "/progress.bin", f)) {
    LOG_ERR("ERS", "Failed to open progress cache: %s", cachePath.c_str());
    return false;
  }

  EpubProgressRecord record;
  record.spineIndex = spineIndex;
  record.page = currentPage;
  record.pageCount = pageCount;
  record.percent = percent;
  record.paragraph = paragraphIndex;
  record.shown = shown;
  uint8_t data[EpubProgressRecord::kMaxSize] = {};
  const size_t size = record.encode(data);
  f.write(data, size);
  f.close();
  return true;
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount,
                                      const std::optional<uint16_t> paragraphIndex,
                                      const std::optional<EpubProgressRecord::Shown>& shown) {
  const uint8_t percent = epubProgressPercentByte(*epub, spineIndex, currentPage, pageCount);
  if (!writeReaderProgressCache(epub->getCachePath(), spineIndex, currentPage, pageCount, percent, paragraphIndex,
                                shown)) {
    LOG_ERR("ERS", "Could not save progress!");
    return;
  }
  // pageCount 0 means the percent is the "unknown" placeholder (see epubProgressPercentByte),
  // e.g. a mid-build page turn. Don't push it into the session tracker: recordSession()
  // overwrites the stored per-book progress, so an unknown 0 at session end would regress it.
  if (pageCount > 0) {
    globalReadingSessionTracker().updateProgress(percent);
  }
  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d (%d%%)", spineIndex, currentPage, percent);
}

void EpubReaderActivity::renderContents(RenderLock& lock, std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  logReaderMemSnapshot("render_start");
  // Read before the pin below overwrites it: this pass redraws the page already on the panel (a
  // clock or battery tick, an overlay closing) rather than showing a new one.
  const bool redrawOfPageOnScreen =
      section && lastRenderedSpineIndex_ == currentSpineIndex && lastRenderedPageIndex_ == section->currentPage;
  // Pin the page identity now, while it still describes what this pass is about to draw.
  if (section) {
    lastRenderedSpineIndex_ = currentSpineIndex;
    lastRenderedPageIndex_ = section->currentPage;
    lastRenderedPageCount_ = section->pageCount;
  }
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(*page, viewportHeight);

  const bool aaEnabledForThisRender =
      getEffectiveTextAntiAliasing() && renderer.hasSecondaryBuffer() && !secondaryBufferDegraded_;
  if (getEffectiveTextAntiAliasing() && !aaEnabledForThisRender) {
    LOG_DBG("ERS", "AA skipped: secondary display buffer unavailable/degraded");
  }
  lastRenderStats.textAntiAliasing = aaEnabledForThisRender;

  // The BW plane always uses 1-bit Atkinson dither. 4-level Bayer values would collapse
  // to black here (DirectPixelWriter draws BW for any value < 3), which is what made the
  // earlier "always Bayer" attempt look wrong.
  const bool imageMonochrome = true;
  // With AA on, the grayscale planes replay a 4-level Bayer cache on top of that BW frame
  // to lift levels 1 and 2 back to real greys. That replay needs its own .bayer.pxc, so
  // warm both variants. If the grayscale pass is preempted or aborted (decided later, at
  // aaPreempted below), the 1-bit BW frame is what stays on the panel — the same output
  // as with AA off, never a crushed-to-black image.
  const bool warmGrayscaleImageCache = aaEnabledForThisRender;

  // Warm any missing image pixel caches BEFORE font prewarm and BW backup chunks
  // reduce heap contig below the ~49 KB the PNG decoder needs. The decode
  // writes pixels into the framebuffer as a side effect, so we reclear before
  // the real BW render begins. Skips when no decode is needed (all images cached
  // or the page is text-only). Mirrors the effectiveForceLoad rule used by the
  // BW render below so placeholder logic is identical.
  //
  // If the secondary frame buffer is allocated (~52 KB on X3, ~48 KB on X4) and
  // the page has images that still need decoding, release it before the warm pass
  // so the PNG/JPEG decoder can use that contiguous block. The secondary buffer
  // is safe to release here because no waveform is pending: displayBuffer() has
  // not been called yet this render cycle. We reallocate after warm completes.
  // This mirrors the same technique used during section indexing (createSectionFile).
  const bool warmForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  bool releasedSecondaryForWarm = false;
  // Set for the whole warm pass (decode included, not just the release/realloc bracket):
  // stepBackgroundSectionBuild()/stepCurrentSectionBuild() check this and refuse to start
  // heap-hungry work while it's true. RenderLock already excludes them structurally (both
  // bail on RenderLock::peek(), and this whole pass runs under the lock the render task took
  // before calling renderContents()) — this flag is a second, explicit guard against that
  // invariant silently breaking if a future change adds a yield point in here.
  // A page the memory ladder gave up on draws its undecoded images as low-memory boxes at once (see the BW render):
  // every decode was refused a moment ago, and a clock tick that redraws the page must not climb the ladder again.
  const bool ladderGaveUp = section && memoryDegradedPage(currentSpineIndex, section->currentPage);
  imageProcessingActive_ =
      !ladderGaveUp && page->hasUncachedImages(warmForceLoad, imageMonochrome, warmGrayscaleImageCache);
  // BORROW the secondary buffer as the decode scratch rather than releasing it. Both give the
  // decoders the same ~52 KB, but the borrowed block never enters the heap:
  //   - it cannot be carved up by the decode's own churn (rule 4 — this pass was the documented
  //     violation: 32 KB ring + 12 KB pool + band + ditherer cycling inside the freed hole);
  //   - returning it cannot fail, so the reallocSecondaryEvictingCaches() below — which evicts
  //     font caches and can escalate to a fragmented-heap RESTART — never runs.
  // The decoders bump-allocate inside it via image_scratch instead of malloc'ing per decode.
  //
  // Release stays as the fallback for when there is nothing to lend (already released for a C
  // build, or no secondary buffer at all), so the previous behaviour is intact on those paths.
  // See docs/memory-allocation-strategy.md §9.3.
  std::unique_ptr<BuildArena> warmScratch;
  bool borrowedSecondaryForWarm = false;
  if (imageProcessingActive_ && renderer.hasSecondaryBuffer()) {
    size_t borrowedSize = 0;
    if (uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedSize)) {
      warmScratch = makeUniqueNoThrow<BuildArena>(borrowed, borrowedSize);
      if (warmScratch && warmScratch->valid()) {
        borrowedSecondaryForWarm = true;
        LOG_DBG("ERS", "Borrowed secondary buffer (%u bytes) as image warm scratch",
                static_cast<uint32_t>(borrowedSize));
      } else {
        warmScratch.reset();
        renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
      }
    }
    if (!borrowedSecondaryForWarm) {
      renderer.releaseSecondaryBuffer();
      releasedSecondaryForWarm = true;
      LOG_DBG("ERS", "Released secondary buffer for image warm pass (nothing to lend)");
    }
  }
  const uint8_t accents = imageProcessingActive_ ? sdFontSystem.bookAccentsLoaded() : 0;
  ImageMemoryLadder::Action ladder = ImageMemoryLadder::Action::Done;
  if (imageProcessingActive_) {
    // Scoped so the arena is uninstalled before the borrow is returned below — a stale
    // image_scratch pointer into a region the display owns again would be a use-after-free.
    image_scratch::ScopedArena warmScratchScope(warmScratch.get());
    ladder = warmImagesClimbingLadder(*page, orientedMarginLeft, contentTop, warmForceLoad, warmGrayscaleImageCache,
                                      warmScratch.get(), /*drawsPage=*/true, accents);
  }
  // The borrow is returned after clearScreen() below, not here: returnSecondaryBuffer() seeds the
  // restored buffer from the live framebuffer, and right now that still holds the decode's
  // side-effect pixels. Returning after the clear seeds it clean instead.
  //
  // Image decode (JPEG/PNG) is the deepest, most heap-hungry work in a render pass
  // and the prime suspect for the lazy multi_heap poisoning assert. Check here, right
  // after the warm/decode pass, so corruption is attributed to the decode rather than
  // to whatever frees next. Mirrors the after_createSectionFile tripwire.
  checkHeapIntegrity("after_image_warm_pass");
  if (releasedSecondaryForWarm) {
    // Safe to evict here: the font prewarm for this page runs AFTER this realloc (image
    // warm is deliberately ordered before it), so cleared glyph slots are rebuilt anyway.
    if (!reallocSecondaryEvictingCaches()) {
      LOG_ERR("ERS", "Failed to reallocate secondary buffer after image warm — display quality degraded");
      secondaryBufferDegraded_ = true;
      const uint32_t freeAfterWarm = esp_get_free_heap_size();
      // See the matching comment in compileSectionCache: do not walk the TLSF free-block
      // list here either, for the same post-decode-failure corruption risk.
      if (maybeRestartForFragmentedHeap(freeAfterWarm, 0)) {
        imageProcessingActive_ = false;
        return;  // fragmented-heap recovery reboot in progress
      }
    }
    // NOTE: do NOT syncRedRamFromFrameBuffer() here. reallocSecondaryBuffer() fills the new
    // secondary with WHITE, and syncRedRamFromFrameBuffer() copies frameBufferActive (that white
    // buffer) into RED RAM — which DESTROYS the correct baseline: RED already holds the previously
    // displayed frame (the page under this one) from its own post-display sync, and _redRamSynced
    // survives release/realloc. Reseeding from the white buffer made the next FAST differential diff
    // the new page against white, so the previous page (e.g. the cover) bled through ("next page
    // overloaded over the cover"). The displayed frame is gone from both host buffers after the
    // release, so the controller's retained RED RAM is the only correct baseline — leave it intact.
  }
  imageProcessingActive_ = false;
  renderer.clearScreen();
  if (borrowedSecondaryForWarm) {
    // Now that the framebuffer is clean, hand the block back. returnSecondaryBuffer() seeds the
    // restored secondary from it and arms the one-shot RED baseline, exactly as
    // reallocSecondaryBuffer() would have — so the release path's careful
    // "do NOT syncRedRamFromFrameBuffer()" reasoning above applies unchanged here. Unlike that
    // path this cannot fail, so there is no eviction and no restart branch.
    warmScratch.reset();
    renderer.returnSecondaryBuffer();
  }
  if (finishLadder(ladder, accents)) {
    lock.unlock();  // renderNormalPass takes it again; the page is drawn once the chapter is laid out again
    return;
  }

  logReaderMemSnapshot("prewarm_begin");

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  prewarmPageFonts(*page, orientedMarginLeft, contentTop);
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  logReaderMemSnapshot("prewarm_end");

  // Single push beats both older strategies wherever the panel offers it: the
  // page arrives complete in one waveform, with no intermediate B/W state and no
  // second refresh. It also has to be chosen HERE rather than at display time,
  // because it changes what happens before the page is even drawn.
  //
  // Nothing here is preemptible any more, and nothing needs to be: the planes
  // are a by-product of the page render rather than ~1 s of extra work, so
  // there is no pass for a page turn to abort.
  const bool singlePushAaThisRender = aaEnabledForThisRender && renderer.supportsGrayFrame();

  const bool effectiveForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  pageHasPlaceholders = page->hasPlaceholderImages(effectiveForceLoad, imageMonochrome);

  // The image-page half refresh is owed to the page AFTER an image page. A redraw of the image page
  // itself must leave it pending: consuming it here flashed a HALF on every clock minute while an
  // image page was on screen (X3, 2026-10-08), and the page re-armed it each time since it still has
  // images. Keyed on the page last drawn, spine included, not on the status-bar tick test below: a
  // one-page chapter followed by the next chapter's page 0 has the same page number, and that turn,
  // from a full-page illustration, is the one this refresh exists for.
  bool forceHalfRefreshThisPage =
      !redrawOfPageOnScreen && pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  if (!redrawOfPageOnScreen) pendingHalfRefreshAfterImagePage = false;
  lastRenderStats.imagePageWithAA = false;
  lastRenderStats.forcedHalfRefresh = forceHalfRefreshThisPage;

  // Single-push AA: the planes are captured BY the page render below, not by two
  // extra renders of their own. Idea from jetaudio's crosspoint-aurora; see
  // GfxRenderer::beginGrayCapture for how it is done here.
  //
  // The buffers are panel-native full-page planes, allocated per render and
  // freed with this scope — together ~130 KB on this panel, which is the whole
  // cost of the approach and why it is gated on supportsGrayFrame(). A board
  // that cannot use them keeps the staged path and never allocates; an
  // allocation failure falls back the same way.
  const size_t capPlaneBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
  std::unique_ptr<uint8_t[]> capLsb, capMsb;
  if (singlePushAaThisRender) {
    capLsb = makeUniqueNoThrow<uint8_t[]>(capPlaneBytes);
    capMsb = capLsb ? makeUniqueNoThrow<uint8_t[]>(capPlaneBytes) : nullptr;
    if (capMsb) {
      memset(capLsb.get(), 0, capPlaneBytes);
      memset(capMsb.get(), 0, capPlaneBytes);
      renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
      renderer.beginGrayCapture(capLsb.get(), capMsb.get());
    } else {
      capLsb.reset();  // OOM: the staged path below runs unchanged
    }
  }

  logReaderMemSnapshot("before_bw_render");
  ImageBlock::takeLowMemoryPlaceholders();  // the warm pass's boxes were cleared with the screen
  {
    // The ladder found no decode fits: the images are boxes, and no decode is tried again on the heap.
    std::optional<ImageBlock::PlaceholderOnlyScope> boxes;
    if (ladderGaveUp || ladder == ImageMemoryLadder::Action::Emergency) {
      boxes.emplace(ImageBlock::Placeholder::LowMemory);
    }
    page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop, effectiveForceLoad,
                 imageMonochrome);
  }
  // An image drawn as a low-memory box: the status bar says so while the page is on screen, and its redraws skip
  // the decodes. Cleared once the page draws without one.
  if (ImageBlock::takeLowMemoryPlaceholders() > 0 && section) {
    memoryDegradedSpine_ = currentSpineIndex;
    memoryDegradedPage_ = section->currentPage;
  } else if (section && memoryDegradedPage(currentSpineIndex, section->currentPage)) {
    memoryDegradedSpine_ = memoryDegradedPage_ = -1;
  }
  // The BW render also touches images (placeholder/cache draws) and runs the full
  // glyph pipeline; check here too so a clean after_image_warm_pass followed by a
  // corrupt reading convicts the BW render rather than the decode.
  checkHeapIntegrity("after_bw_render");
#if DEBUG_BACKGROUND_WORK
  // This page was rendered fresh on the render task (not served from a Background-A
  // pre-render). Mark the overlay as a miss before the status bar draws it.
  backgroundAGlyph_ = '-';
#endif
  publishPageLinkTargets(*page, orientedMarginLeft, contentTop);
  // Snapshot the page the status bar last reported BEFORE renderStatusBar()
  // overwrites it: further down it is the only way left to tell a page turn from
  // a clock or battery tick that redrew the page already on the panel.
  const int statusBarPageBeforeRender = lastStatusBarPage;
  renderStatusBar();
  if (showTruncatedSectionHintThisRender) {
    const int hintX = orientedMarginLeft + 4;
    const int hintY1 = contentTop + 4;
    const int hintY2 = hintY1 + 20;
    const int maxWidth = std::max(0, renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight - 8);
    // Clear a dedicated band so the hint stays readable over any page content.
    const int boxX = hintX - 2;
    const int boxY = hintY1 - 2;
    const int boxW = maxWidth + 4;
    const int boxH = 44;
    renderer.fillRect(boxX, boxY, boxW, boxH, false);
    renderer.drawRect(boxX, boxY, boxW, boxH, true);
    const std::string line1 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_1, maxWidth);
    const std::string line2 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_2, maxWidth);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY1, line1.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY2, line2.c_str(), true);
  }
  // Everything the capture was meant to see is drawn. The status bar is inside
  // the window on purpose and costs nothing: UI chrome is 1-bit, so it produces
  // no anti-aliased pixels for the capture to record.
  renderer.endGrayCapture();
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  logReaderMemSnapshot("after_bw_render");
  // Resolve this page's refresh mode (consuming any force flags), then fire it.
  // With AA enabled on X4 the refresh goes out async so the grayscale planes
  // can render during the waveform (inline AA below); everywhere else the
  // trigger blocks through the waveform exactly as before.
  HalDisplay::RefreshMode pageRefreshMode;
  // A buffer lent for a build-through is not a degraded one: the X3 diffs against its own copy of
  // the frame on screen, so it keeps the normal cadence (see BG_BUILD_THROUGH_PAGES).
  if (secondaryBufferDegraded_ && !backgroundBuildThrough_) {
    // FULL_REFRESH already gives a clean baseline, same goal as forceHalfRefreshAfterPopup_;
    // consume it here too so it doesn't carry over and force an unrelated later page to HALF.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = HalDisplay::FULL_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceRefreshModeNextRender_ >= 0) {
    // Manual force-refresh button: apply the requested mode for this one render. A manual refresh
    // gives its own clean baseline, so consume any armed post-popup HALF too rather than letting it
    // carry over and force an unrelated later page to HALF.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = static_cast<HalDisplay::RefreshMode>(forceRefreshModeNextRender_);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    forceRefreshModeNextRender_ = -1;
  } else if (forceHalfRefreshAfterPopup_) {
    // First real page after the indexing popup, shown directly here because the build finished
    // in a single slice (e.g. a one-page cover) and never went through displayBuildPage(). See
    // forceHalfRefreshAfterPopup_.
    forceHalfRefreshAfterPopup_ = false;
    pageRefreshMode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (forceHalfRefreshThisPage) {
    pageRefreshMode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (statusBarPageBeforeRender >= 0 && section && section->currentPage + 1 == statusBarPageBeforeRender) {
    // Same page as the one already on the panel: this render was a status-bar
    // tick (clock minute or battery step), not a page turn.
    //
    // refreshFrequencyPages is a GHOSTING BUDGET DENOMINATED IN PAGES -- the
    // setting, its label and the counter all say pages. Letting a tick spend
    // from it drains the budget once a minute whether or not anyone is reading,
    // so on an idle reader the ghost-clearing HALF fires on a schedule set by
    // the clock rather than by page turns: ~2s of full-screen flash caused by a
    // digit changing. Device-traced on an X4 2026-09-16, counter 10 -> 9 -> 8
    // across three consecutive minutes with no page turns.
    //
    // The tick still costs a FAST refresh of the whole frame, which is not
    // free -- a no-diff FAST accumulates speckle on X3 (see the
    // lastStatusBar* field comment). What it must not do is bill that to the
    // page budget.
    pageRefreshMode = HalDisplay::FAST_REFRESH;
  } else {
    pageRefreshMode = ReaderUtils::nextRefreshCycleMode(pagesUntilFullRefresh);
  }
  // Inline AA is X4-only: X4 waits out the waveform inside the trigger, so the
  // async split hands that window to the plane renders. X3 returns pre-waveform
  // from its trigger already and keeps the deferred loop-task pass.
  //
  // The inline pass runs under the render lock with no cancellation point, and it is not
  // cheap: only its LSB plane render overlaps the waveform, while the plane SPI writes, the
  // gray flush and the restore write are all additive. So drop it outright when the page is
  // already on its way out — otherwise a burst of turns pays that in full for every
  // intermediate page it immediately discards. Evaluated here, as late as possible, so a
  // gesture that begins during the (long) BW render still counts.
  //
  // X3's deferred pass needs no equivalent gate and keeps its original arming: it is only
  // ever run from the idle loop (which a pending turn makes unreachable) and pageTurn()
  // clears it, so it is already cancellable by design.
  // Inline AA only makes sense on a panel that can actually overlap: its whole
  // premise is that the LSB plane render happens *inside* a running waveform. Ask
  // the driver, not the board.
  //
  // This was `!renderer.isX3()`, i.e. "everything that is not an X3 is an X4".
  // On the T5S3 that is false twice over: LgfxEpdDriver does not override
  // PanelDriver::supportsAsyncDisplay() and so cannot overlap at all, yet the
  // board took the inline path and paid the plane / gray flush / restore writes
  // additively, with a blocking refresh underneath. A suspected contributor to the
  // ghosting seen there.
  //
  // X3 is still excluded by name: it *can* refresh asynchronously, but keeps its
  // deferred pass for scheduling reasons of its own (the _refreshPending guards
  // above). That is a separate question and needs its own predicate before the
  // name can go.
  const bool inlineAaAvailable = !usesDeferredAa();
  const bool aaPreempted = aaEnabledForThisRender && inlineAaAvailable && aaPreemptedByNavigation();
  if (aaPreempted) {
    LOG_DBG("ERS", "Inline AA skipped: page preempted by navigation");
  }
  const bool inlineAaThisRender =
      aaEnabledForThisRender && !singlePushAaThisRender && inlineAaAvailable && !aaPreempted;
  // Anything that cannot run AA inline defers it, rather than only the X3 doing so.
  const bool deferredAaThisRender = aaEnabledForThisRender && !singlePushAaThisRender && !inlineAaAvailable;
  lastRenderStats.textAntiAliasing = inlineAaThisRender || deferredAaThisRender;
  if (capLsb) {
    // Hand the captured planes to the controller, then base + greys go out as
    // one waveform. Cheap: the planes were filled by the page render that just
    // ran, so nothing is walked twice.
    renderer.copyGrayscaleLsbBuffers(capLsb.get());
    renderer.copyGrayscaleMsbBuffers(capMsb.get());
    // Base and greys as ONE waveform. Deferred, not blocking: there is no AA work left to
    // overlap -- the planes went out with the page -- but there is plenty of OTHER work, and it
    // was all sitting behind this call. The pre-render scheduling and lock.unlock() below exist
    // precisely to hand the waveform window to the loop task, and a blocking display here meant
    // the waveform was already over by the time they ran. completeDisplay() after the unlock
    // collects it, exactly as the trigger paths do.
    //
    // Measured on a T5S3, where this is the only path that advertises supportsGrayFrame():
    // 1.4-2.7 s per page turn during which stepCurrentSectionBuild() could not run at all.
    renderer.triggerGrayscaleFrame(pageRefreshMode);
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.textAntiAliasing = true;
  } else if (inlineAaThisRender) {
    renderer.triggerDisplayAsync(pageRefreshMode);
  } else {
    renderer.triggerDisplay(pageRefreshMode);
  }
  // Real content is now on screen; any indexing popup it replaced is gone. Clear the flag for the
  // abandon-to-adjacent-section path, which reaches this Normal pass without going through
  // renderSectionBuildingPass()/displayBuildPage() where buildingPopupShown_ is otherwise reset.
  buildingPopupShown_ = false;
  // Capture this page's refresh mode/byte NOW, before the lock is released and the deferred-AA
  // grayscale display overwrites the renderer's last-mode (which would otherwise mislabel the
  // page summary logged afterwards).
  lastPageRefreshMode_ = renderer.getLastRefreshMode();
  lastPageDisplayModeByte_ = renderer.getLastDisplayModeByte();
  const auto tDisplay = millis();

  // Schedule a half-refresh on the next page turn after an image page to reduce ghosting.
  // Must be checked BEFORE page is moved into pendingGrayscale_ below.
  if (page->hasImages() && !page->allImagesArePlaceholders(effectiveForceLoad, imageMonochrome) &&
      getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  if (singlePushAaThisRender) {
    // Nothing to schedule and nothing to restore: the planes were staged before
    // the page render and went out with it in one waveform. No deferred pass, so
    // no second refresh for the idle loop to run and nothing for a page turn to
    // preempt half-way.
    LOG_DBG("ERS", "Single-push AA (captured): render=%lums display=%lums", tBwRender - tPrewarm, tDisplay - tBwRender);
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.textAntiAliasing = true;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, 0, 0, 0, 0, millis() - t0};
  } else if (inlineAaThisRender) {
    // Inline AA (X4): the BW waveform is still running from triggerDisplayAsync().
    // Render the grayscale planes now — the LSB plane lands inside the waveform
    // window, so after the wait only the LSB SPI write, the MSB plane and the
    // short gray flush remain. The AA touch-up then reads as the tail of the
    // page refresh instead of a separate later update (issue #71).
    renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
    const int aaFontId = getEffectiveReaderFontId();
    const Page* pagePtr = page.get();
    // The per-element abort buys no wall-clock here — this plane render is overlapped with the
    // BW waveform, so bailing early just idles in finishDisplayAsync() for the remainder. It is
    // still worth setting: latching planeAborted means the pass aborts even if the input signal
    // has been drained by the loop task before the predicate below is evaluated.
    bool planeAborted = false;
    const auto gt = renderer.renderGrayscalePlanesInterleaved(
        [&](GfxRenderer::RenderMode) {
          if (!pagePtr->renderTextOnly(renderer, aaFontId, orientedMarginLeft, contentTop, /*abortable=*/true)) {
            planeAborted = true;
            return;
          }
          pagePtr->renderImagesFromGrayscaleCache(renderer, orientedMarginLeft, contentTop);
        },
        // Re-checked here because the up-front gate can only see the gesture as it stands when
        // the render begins; the pass itself is ~1 s long, so most presses during a burst land
        // after that decision and would otherwise be unstoppable.
        [&] { return planeAborted || aaPreemptedByNavigation(); });
    LOG_DBG("ERS", "Inline AA%s: planes=%lums gray=%lums restore=%lums", gt.aborted ? " ABORTED" : "", gt.planesMs,
            gt.displayMs, gt.restoreMs);
    checkHeapIntegrity("after_inline_aa");
    lastRenderStats.usedGrayscale = !gt.aborted;
    lastRenderStats.textAntiAliasing = !gt.aborted;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, gt.planesMs, 0,
                              gt.displayMs,  gt.restoreMs,         millis() - t0};
  } else if (deferredAaThisRender) {
    // Deferred grayscale: store context before releasing the lock, so loop() can
    // run the AA pass once the waveform ends. The page is kept alive via
    // shared_ptr. Used by the X3 and by any panel that cannot overlap the plane
    // work with the waveform (see inlineAaAvailable above) -- the machinery here
    // is board-agnostic; only the choice to use it was ever board-specific.
    pendingGrayscale_.active = true;
    pendingGrayscale_.page = std::move(page);
    markStagedForCurrentPage();
    pendingGrayscale_.fontId = getEffectiveReaderFontId();
    pendingGrayscale_.marginLeft = orientedMarginLeft;
    pendingGrayscale_.contentTop = contentTop;
    pendingGrayscale_.fastLut = SETTINGS.fastAntiAliasing;
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, 0, 0, 0, 0, tDisplay - t0};
  }

  // Collect font stats before releasing the lock (these read renderer state).
  if (const auto* cacheManager = renderer.getFontCacheManager()) {
    if (const auto* decompressor = cacheManager->getDecompressor()) {
      const auto& stats = decompressor->getStats();
      lastRenderStats.fontCacheHits = stats.cacheHits;
      lastRenderStats.fontCacheMisses = stats.cacheMisses;
      lastRenderStats.fontDecompressMs = stats.decompressTimeMs;
      lastRenderStats.fontUniqueGroups = stats.uniqueGroupsAccessed;
      lastRenderStats.fontPageBufferBytes = stats.pageBufferBytes;
      lastRenderStats.fontPageGlyphsBytes = stats.pageGlyphsBytes;
      lastRenderStats.fontPeakTempBytes = stats.peakTempBytes;
      lastRenderStats.fontGetBitmapTimeUs = stats.getBitmapTimeUs;
      lastRenderStats.fontGetBitmapCalls = stats.getBitmapCalls;
    }
  }

  // Schedule the pre-render BEFORE releasing the lock so the loop task can
  // execute it during the waveform wait (~2-4s on X3). frameBuffer is already
  // swapped — the loop task writes into the new (inactive) buffer while the
  // display controller scans the old one. If a page turn fires during the
  // waveform it will clear preRenderedPage.ready and the stale pre-render
  // result is discarded — no correctness issue.
  // Not during a build-through: on the X3 the post-waveform frame sync reads this very buffer
  // when there is no second one (see BG_BUILD_THROUGH_PAGES).
  if (!preRenderedPage.ready && !backgroundBuildThrough_ && section && section->currentPage + 1 < section->pageCount) {
    pendingPreRender = true;
    markStagedForCurrentPage();
    // Do NOT request the update while a deferred AA is owed: isUpdateSuperseded()
    // is true from the moment requestUpdate() sets its flag, and
    // aaPreemptedByNavigation() reads that as "this page is on its way out", so
    // the AA armed a few lines above would abort before rendering anything
    // useful. The post-AA hook in runDeferredGrayscalePass() re-requests it once
    // the pass has finished, which is what pendingPreRender is carried for.
    //
    // The X3 only survived this by timing: it holds _refreshPending for its
    // multi-second waveform, so the render task consumed this request and the
    // "PreRender deferred: AA owed" guard shelved it before the pass could start.
    // A panel whose refresh completes promptly (T5S3) starts the AA while the
    // flag is still set and kills it -- every page, "gray=0ms", ~90 ms of plane
    // work discarded and no anti-aliasing on screen at all.
    if (!pendingGrayscale_.active) {
      requestUpdate();
    }
  }

  // Release the render lock NOW — the waveform is running in hardware and
  // frameBuffer is already swapped. The loop task gets full CPU during the
  // waveform for input handling and pre-render scheduling.
  lock.unlock();

#ifdef ERS_TEST_WIDEN_RENDER_WINDOW_MS
  // TEST HOOK — never defined by any environment in platformio.ini. Holds the mid-render
  // unlocked window open so a transition requested during it deterministically overlaps a live
  // render pass, which is otherwise a ~1-2 s target that has to be hit by hand. Validates
  // RenderLock(ExclusiveActivityAccess): with the guard, the transition logs
  // "Transition waited Nms" and proceeds safely; swap the two ExclusiveActivityAccess call
  // sites in ActivityManager::loop() back to the plain constructor and the same gesture runs
  // the activity's destructor while this pass is still using `this`.
  // Build with: PLATFORMIO_BUILD_FLAGS=-DERS_TEST_WIDEN_RENDER_WINDOW_MS=4000 pio run
  LOG_INF("ERS", "TEST: holding the mid-render unlocked window open for %d ms", ERS_TEST_WIDEN_RENDER_WINDOW_MS);
  delay(ERS_TEST_WIDEN_RENDER_WINDOW_MS);
#endif

  // Sleep until BUSY deasserts, then do post-waveform SPI work (DTM1 resync
  // on X3, conditioning passes, flag updates). SPI ownership transfers back
  // to this task only after completeDisplay() returns.
  renderer.completeDisplay();
  // Close the wake trace only now. triggerDisplay() returns once the content is COMMITTED, not
  // once the pixels have settled — on X3 the first post-wake refresh is a mandatory full sync
  // whose three waveform passes run after it returns. Stamping at the trigger understated the
  // measured wake by ~2.1 s (trace said 858 ms while the page landed at ~2.4 s), which is
  // exactly the number this trace exists to get right.
  WakeTrace::mark(WakeTrace::Phase::PageVisible);
  WakeTrace::logSummary();
}

void EpubReaderActivity::displayBuildPage(RenderLock& lock, const Page& page, const RenderLayout& layout,
                                          BuildArena::Block* drawBlock) {
  // Draws one page from an in-progress Background-C build: a plain BW render + status bar, no
  // AA and no pre-render arming (those belong to the steady reading state set up by
  // renderNormalPass() once the build completes). Images come from their pixel cache when they
  // have one and are drawn as "indexing" placeholders otherwise (ImageBlock::PlaceholderOnlyScope
  // below), so no decode runs on the build's heap and no secondary-buffer release is needed. The lock is released
  // before the waveform wait — exactly like renderContents() — so a C build slice can run on the loop task during the
  // refresh.
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - layout.marginTop - layout.marginBottom);
  const int contentTop = layout.marginTop + getImageOnlyPageYOffset(page, viewportHeight);

  // Everything that touches the build's arena -- the font-slot scope below and the caller's
  // page block -- ends inside this brace, BEFORE lock.unlock(): a build slice runs on the loop
  // task during the waveform wait, and with per-line arena allocation in the parser its lines
  // would land above a still-open block and be rewound with it. (The slot scope used to close at
  // function exit, after the unlock; harmless while the parser made no arena allocations in
  // phase (b), a corruption once it did -- memory audit 2026-09, R2 step 2b.)
  {
    auto* fcm = renderer.getFontCacheManager();
    // Draw this page's font slots from the build's own arena rather than the heap. Measured X3
    // 2026-08-11: the prewarm's six blocks (~9 KB) cost contig 36852 -> 27636 here, and releasing
    // them afterwards returned every byte and every block while contig did not move at all — the
    // bytes were never the problem, their placement was (§9.2.4). The borrowed region they come
    // from is the one the build already holds and is barely using (highWater 12864-28656 of
    // 52272), and nothing else can allocate inside it.
    //
    // Only while the buffer is actually lent; otherwise buildScratch_ is null and the scope
    // self-disables, leaving the heap path exactly as it was.
    FontCacheManager::ScopedSlotArena slotArena(*fcm, secondaryBorrowed_ ? buildScratch_.get() : nullptr);
    prewarmPageFonts(page, layout.marginLeft, contentTop);

    renderer.clearScreen();
    {
      ImageBlock::PlaceholderOnlyScope placeholders;
      page.render(renderer, getEffectiveReaderFontId(), layout.marginLeft, contentTop,
                  /*forceLoadLargeImages=*/false, /*monochromeOutput=*/true);
    }
    publishPageLinkTargets(page, layout.marginLeft, contentTop);
    renderStatusBar();
    if (forceHalfRefreshAfterPopup_) {
      // First real page after the indexing popup: establish a clean baseline (see
      // forceHalfRefreshAfterPopup_) instead of compounding onto the popup's FAST refresh.
      forceHalfRefreshAfterPopup_ = false;
      renderer.triggerDisplay(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      ReaderUtils::triggerWithRefreshCycle(renderer, pagesUntilFullRefresh);
    }
    // Hand the font page slots back before the build resumes. This is THE allocation that makes a
    // mid-build draw expensive, and it is expensive because of where it lands, not what it costs:
    // prewarm takes ~8.9 KB of page buffer + glyph table (pageBuf 7635 + pageGlyphs 1264 measured,
    // across the 3 style slots a page uses) out of the middle of the largest free block, while the
    // build's working set already occupies the rest of the heap. Device-measured X3 2026-08-11:
    // contig 40948 -> 23540 across this one draw, never recovering for the remainder of the parse,
    // which then ran in "continuing in degraded mode" throughout and left contig 1036 bytes short
    // of Background-B's floor afterwards.
    //
    // Normally the slots live on until the next prewarm replaces them, which is free — but here
    // "until the next prewarm" spans the rest of a multi-second build. Releasing now lets the block
    // coalesce before stepCurrentSectionBuild() resumes; nothing else allocates in between, because
    // the lock is still held.
    //
    // When the slots came from the arena instead (the normal case now — see ScopedSlotArena above)
    // this returns nothing to the heap, because they never came from it; the arena scope rewinds
    // them on the way out of this function. Kept unconditional because it is still the right thing
    // on the heap path, which is what runs whenever the buffer is not lent.
    //
    // Costs nothing: prewarmCache() frees and rebuilds a slot's buffer on every call anyway
    // (FontDecompressor.cpp, "Roll back this slot only"), so no work is thrown away that the next
    // page would not have redone. Safe here because every glyph consumer for this page has already
    // run — the scan pass, page.render() and renderStatusBar() are all above, and a mid-build draw
    // has no AA pass to replay later (the borrowed buffer forces secondaryBufferDegraded_).
    renderer.getFontCacheManager()->clearCache();
  }  // slot scope closed
  if (drawBlock != nullptr && drawBlock->valid() && buildScratch_) {
    // The page's bytes are no longer read (render and link targets are done above); the caller
    // still owns the object, whose destructor does not touch them.
    if (!buildScratch_->release(*drawBlock)) {
      LOG_ERR("ERS", "Mid-build draw block could not be released (out of order)");
    }
  }

  // Release the lock before the (blocking) waveform wait so stepCurrentSectionBuild() can run a
  // build slice on the loop task while the panel refreshes — the same hand-off renderContents()
  // uses for Background-A.
  lock.unlock();
  renderer.completeDisplay();
  // On a cache miss this mid-build page is what the user actually sees first, well before the
  // build completes and renderNormalPass() runs — so the trace has to close here or a miss
  // would report a wake latency that includes the whole rest of the build. After
  // completeDisplay() for the same reason as renderContents(): the pixels are settled here.
  WakeTrace::mark(WakeTrace::Phase::PageVisible);
  WakeTrace::logSummary();
}

uint16_t EpubReaderActivity::renderPageContentOnly(const Page& page, const int orientedMarginTop,
                                                   const int orientedMarginRight, const int orientedMarginBottom,
                                                   const int orientedMarginLeft) {
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

  prewarmPageFonts(page, orientedMarginLeft, contentTop);

  // On a single-push panel the pre-render owes the SAME thing a fresh render
  // produces, or a pre-rendered page would look different from one rendered on
  // the spot — and page turns hit the pre-render almost every time, so that
  // difference would be the normal case rather than the exception.
  //
  // The planes have to be staged HERE and not at display time: both they and the
  // page use the framebuffer as scratch, so whichever runs last owns it, and the
  // page has to be the survivor. At display time the pre-rendered page is already
  // in that buffer and staging would destroy it.
  // Same capture the foreground render uses, so a pre-rendered page and a fresh
  // one are produced identically — see the single-push block in render().
  // Buffers live only for this function; the planes are handed to the controller
  // before it returns and the display consumes them at the page turn.
  preRenderedPlanesStaged_ = false;
  std::unique_ptr<uint8_t[]> capLsb, capMsb;
  const bool wantCapture = renderer.supportsGrayFrame() && getEffectiveTextAntiAliasing() && !page.hasImages();
  if (wantCapture) {
    const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
    capLsb = makeUniqueNoThrow<uint8_t[]>(planeBytes);
    capMsb = capLsb ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    if (capMsb) {
      memset(capLsb.get(), 0, planeBytes);
      memset(capMsb.get(), 0, planeBytes);
      renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
      renderer.beginGrayCapture(capLsb.get(), capMsb.get());
    } else {
      capLsb.reset();
    }
  }

  renderer.clearScreen();
  page.render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
  renderer.endGrayCapture();
  if (capLsb) {
    renderer.copyGrayscaleLsbBuffers(capLsb.get());
    renderer.copyGrayscaleMsbBuffers(capMsb.get());
    preRenderedPlanesStaged_ = true;
  }
  // Status bar intentionally omitted — superimposed at display time with live
  // values, and so outside the capture. It carries no anti-aliasing either way:
  // UI chrome is 1-bit and produces no AA pixels at all.
  const FontDecompressor* fdc = fcm->getDecompressor();
  return fdc ? fdc->getStats().fallbackOomGlyphs : 0;
}

bool EpubReaderActivity::usesDeferredAa() const {
  // Inline AA hides its LSB plane render inside a running waveform, so it needs a
  // panel that can actually overlap. The X3 can, but defers anyway: its async refresh
  // holds the frame buffer, and stays pending, for the whole waveform (see the header
  // and the pre-render guards).
  return !(renderer.supportsAsyncRefresh() && !renderer.asyncRefreshHoldsFrameBuffer());
}

bool EpubReaderActivity::aaPreemptedByNavigation() const {
  // 1) An update is already queued — a turn has completed and its render is waiting.
  if (isUpdateSuperseded()) {
    return true;
  }
  // 2) The sampler has seen a press the loop task has not drained yet. Covers the first
  //    ~10 ms of a gesture, before the press reaches the live-state snapshot below.
  if (mappedInput.hasPendingInput()) {
    return true;
  }
  // 3) A navigation button is down. This is the window the queued-update signal alone
  //    misses: a page turn is only classified on RELEASE, so between press and release
  //    nothing else indicates that the page on screen is about to be replaced — which is
  //    exactly when the AA pass would start and then be unstoppable.
  //
  //    Bounded by LONG_PRESS_MS: past that threshold the Long event has already fired and
  //    been acted on (chapter skip, or whatever the button is mapped to), so continuing to
  //    hold no longer predicts a further turn. Without the bound, a chapter skip performed
  //    with the button still held would suppress the AA of the page it lands on and nothing
  //    would render it again.
  using B = MappedInputManager::Button;
  const bool navigationHeld = mappedInput.isPressed(B::PageForward) || mappedInput.isPressed(B::PageBack) ||
                              mappedInput.isPressed(B::Left) || mappedInput.isPressed(B::Right);
  return navigationHeld && mappedInput.getHeldTime() < ButtonEventManager::LONG_PRESS_MS;
}

void EpubReaderActivity::displayPreRenderedPage(const Page& page, const int orientedMarginTop,
                                                const int orientedMarginRight, const int orientedMarginBottom,
                                                const int orientedMarginLeft) {
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

#if DEBUG_BACKGROUND_WORK
  // This page is being served from the Background-A pre-render buffer (a hit) — set the
  // overlay glyph BEFORE renderStatusBar() draws it, so the status bar reflects that
  // background rendering produced this page.
  backgroundAGlyph_ = 'x';
#endif
  // The pixels came from the pre-render, but the Page is in hand here and the links have to
  // describe what is going on screen NOW — the pre-render measured the page AFTER this one.
  publishPageLinkTargets(page, orientedMarginLeft, contentTop);
  renderStatusBar();

  // The image-page follow-up half-refresh can carry over if the previous page had images; honour
  // and clear it here so the pre-render path doesn't skip it.
  const bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  // Single push when the pre-render staged this page's planes: base and greys go
  // out as one waveform, exactly as a freshly rendered page does. Anything else
  // falls through to the B/W display plus the two-push AA replay below.
  const bool singlePushThisPage = preRenderedPlanesStaged_ && !secondaryBufferDegraded_;
  preRenderedPlanesStaged_ = false;
  // Lent for a build-through rather than lost to an OOM: the X3 diffs against its own copy of
  // the frame on screen, so the normal cadence stays correct (and AA stays off below).
  if (secondaryBufferDegraded_ && !backgroundBuildThrough_) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else if (singlePushThisPage) {
    HalDisplay::RefreshMode mode;
    if (forceHalfRefreshThisPage) {
      mode = HalDisplay::HALF_REFRESH;
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      mode = ReaderUtils::nextRefreshCycleMode(pagesUntilFullRefresh);
    }
    renderer.displayGrayscaleFrame(mode);
    LOG_DBG("ERS", "Single-push AA (pre-rendered)");
  } else if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  // A pre-rendered page can be the first one a book open puts on screen (Background-A having
  // won the race), so this path closes the trace too. Correct to stamp here without a
  // completeDisplay(): unlike renderContents()'s trigger/complete split, displayBuffer() is
  // the blocking variant and has already waited out the waveform by this line.
  WakeTrace::mark(WakeTrace::Phase::PageVisible);
  WakeTrace::logSummary();
  // Capture before the AA replay below overwrites the renderer's last-mode (see renderContents).
  lastPageRefreshMode_ = renderer.getLastRefreshMode();
  lastPageDisplayModeByte_ = renderer.getLastDisplayModeByte();

  // The image-page bookkeeping renderContents() does, in the form it takes for a pre-rendered
  // page: the pre-render shows a page only when every image on it replayed from its cache, so
  // none is a placeholder, and the half refresh that clears an image's grey particles is armed
  // for the page after it whenever it has images at all.
  pageHasPlaceholders = false;
  if (page.hasImages() && getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  // Same gate as renderContents(), and this is the path that matters most for it: a fast
  // forward burst is exactly what hits the pre-render cache, and the replay below costs a
  // glyph re-warm scan, two plane renders, the plane SPI writes and a gray flush — all on a
  // page the next turn is about to replace. It is inline and uncancellable on both devices,
  // so unlike renderContents() this gate applies to X3 too. The BW refresh above has already
  // completed, so checking here also catches a gesture that began during it. Skipping leaves
  // precisely the state the AA-disabled path leaves (no plane render means nothing for the
  // grayscale cleanup to undo), so no buffer or RED-RAM baseline is disturbed.
  const bool aaPreempted = aaPreemptedByNavigation();
  if (aaPreempted) {
    LOG_DBG("ERS", "AA replay skipped: page preempted by navigation");
  }
  if (!singlePushThisPage && !aaPreempted && getEffectiveTextAntiAliasing() && renderer.hasSecondaryBuffer() &&
      !secondaryBufferDegraded_) {
    const int fontId = getEffectiveReaderFontId();
    // Re-warm the page's glyph BITMAPS before the AA replay. The pre-render pass warmed
    // them, but background work since may have dropped or re-wired the cache (B's
    // build slices reset the font accumulation to the metadata-only flash tables) —
    // and replaying against a metadata-only table dereferences a null bitmap base
    // (observed: Load access fault at glyph->dataOffset + heap corruption). When the
    // cache is still warm this is nearly free via the prewarm coverage fast-path.
    prewarmPageFonts(page, orientedMarginLeft, contentTop);
    renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
    // No waveform to hide behind on this path (displayBuffer() above already returned), so the
    // per-element abort translates directly into saved wall-clock on both devices.
    bool planeAborted = false;
    const auto gt = renderer.renderGrayscalePlanesSequential(
        [&](GfxRenderer::RenderMode) {
          if (!page.renderTextOnly(renderer, fontId, orientedMarginLeft, contentTop, /*abortable=*/true)) {
            planeAborted = true;
            return;
          }
          page.renderImagesFromGrayscaleCache(renderer, orientedMarginLeft, contentTop);
        },
        [&] { return planeAborted || aaPreemptedByNavigation(); });
    if (gt.aborted) {
      LOG_DBG("ERS", "AA replay aborted mid-pass: page preempted by navigation");
    }
    // timings not otherwise recorded for the pre-rendered path
  }
  checkHeapIntegrity("after_bufferdisplay_aa");
}

void EpubReaderActivity::appendDebugState(std::string& out) const {
  using SerialCommandGrammar::appendField;
  // 1-based throughout, as the screen counts: spine item n of the book, page n of that item.
  char number[24];
  appendField(out, "book", epub ? epub->getPath() : std::string("none"));
  snprintf(number, sizeof(number), "%d/%d", currentSpineIndex + 1, epub ? epub->getSpineItemsCount() : 0);
  appendField(out, "spine", number);
  snprintf(number, sizeof(number), "%d/%d", section ? section->currentPage + 1 : 0, section ? section->pageCount : 0);
  appendField(out, "page", number);
#if SERIAL_TEST_API
  std::string family;
  resolveReaderFont(&family);
  appendField(out, "font", family);
  appendField(out, "font_pt",
              static_cast<long>(bookFontSizeOverride >= 0 ? bookFontSizeOverride : SETTINGS.fontPointSize));
  if (!epub || !section) return;
  // What the status bar shows, whichever parts of it the settings turn on: the chapter title, the
  // page within the whole TOC chapter (which can span several spine items, so it can differ from
  // page=), and the book percentage.
  appendField(out, "chapter", chapterTitle());
  const ChapterPageSpan::Display chapter = chapterPageDisplay();
  snprintf(number, sizeof(number), "%d/%d%s", chapter.page, chapter.total, chapter.approximate ? "~" : "");
  appendField(out, "chapter_page", number);
  snprintf(number, sizeof(number), "%.0f", statusBarBookPercent());
  appendField(out, "percent", number);
#endif
}

#if SERIAL_TEST_API
bool EpubReaderActivity::hasPendingScreenWork() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;  // a render pass or a build slice has it: busy by definition
  // The section on screen still being built (its pages appear as they are laid out), a page turn
  // handed to the render task, a deferred anti-aliasing redraw of the page on screen.
  return !section || readerPhase_ != ReaderPhase::READING || section->hasActiveBuild() || usePreRenderedBuffer ||
         pendingGrayscale_.active;
}

bool EpubReaderActivity::hasPendingBackgroundWork() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return true;  // a render pass or a build slice has it: busy by definition
  if (pendingPreRender || usePreRenderedBuffer || pendingGrayscale_.active || imageProcessingActive_) return true;
  if (section && section->hasActiveBuild()) return true;
  if (backgroundBuildState_ == BackgroundBuildState::Building) return true;
  if (!epub || !section || readerPhase_ != ReaderPhase::READING || backgroundWorkSuspended_) return false;
  // Work that has not started yet but will on its own. Background-B waits out its quiet period
  // after a page appears (and rechecks its gates once a second) before it borrows the buffer; past
  // that, a WaitHeap the heap keeps refusing is starved, not pending.
  const unsigned long sinceActivity = millis() - std::max(lastPageTurnTime, lastPageOnScreenMs_);
  if (backgroundBuildState_ == BackgroundBuildState::WaitHeap &&
      (!backgroundBuildStarved_ || sinceActivity < BG_BUILD_BORROW_QUIET_MS + 1100UL)) {
    return true;
  }
  // The image lane looks at the pages around the reader once after each move and marks the window
  // clean when nothing is left to decode; until then it has its scan (and maybe a decode) to do.
  // The same preconditions as stepImageWarmLocked(), without which it never runs.
  const bool imageLaneCanRun =
      section->pageCount > 0 && !secondaryBorrowed_ && !backgroundBorrowActive_ && renderer.hasSecondaryBuffer();
  return imageLaneCanRun && (imageWarmCleanSpine_ != currentSpineIndex || imageWarmCleanPage_ != section->currentPage);
}
#endif  // SERIAL_TEST_API

void EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered() {
  if (!preRenderedPage.ready || !section || !epub) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);
  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  auto p = section->loadPageFromSectionFile();
  if (!p) {
    return;
  }
  renderPageContentOnly(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  // The page on the panel has its status bar; without it a screenshot, the sleep overlay and the
  // light drawer all showed the page bare.
  renderStatusBar();
  preRenderedPage.ready = false;
  preRenderedPlanesStaged_ = false;
  pendingPreRender = false;
  usePreRenderedBuffer = false;
}

float EpubReaderActivity::statusBarBookPercent() const {
  // Calculate progress in book. During an active section build pageCount only reflects pages
  // built so far, not the final chapter length, so show a byte-based estimate ("page X of ~Y")
  // instead of the misleading watermark. estimatedTotalPages() returns 0 while it's still too
  // early to project, which suppresses the fraction/progress (same as a plain pageCount of 0).
  const bool building = section->hasActiveBuild();
  const int currentPage = section->currentPage + 1;
  const int displayPageCount = building ? section->estimatedTotalPages() : section->pageCount;
  const float pageCount = static_cast<float>(displayPageCount);
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  return epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;
}

std::string EpubReaderActivity::chapterTitle() const {
  const int tocIndex =
      section ? section->getTocIndexForPage(section->currentPage) : epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex == -1) return tr(STR_UNNAMED);
  return epub->getTocItem(tocIndex).title;
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section->currentPage + 1;
  const float bookProgress = statusBarBookPercent();
  // The page counter and the chapter progress bar count the whole TOC chapter, which can span
  // several spine items (#325). The book percentage above stays per spine.
  const ChapterPageSpan::Display chapter = chapterPageDisplay();
  lastStatusBarChapterTotal_ = chapter.total;
  lastStatusBarChapterApprox_ = chapter.approximate;

  std::string title;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterTitle();
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const bool isStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                      static_cast<uint16_t>(section->currentPage));
  const std::string printedPageLabel = section && SETTINGS.statusBarPrintedPage
                                           ? printedPageLabelFor(static_cast<uint16_t>(section->currentPage))
                                           : std::string();
#if CROSSPOINT_KOREADER_AUTOSYNC
  const SyncIndicator syncIndicator = autoSyncIndicator;
#else
  const SyncIndicator syncIndicator = SyncIndicator::None;
#endif
  GUI.drawStatusBar(renderer, bookProgress, chapter.page, chapter.total, title, 0, isStarred, printedPageLabel,
                    /*fillMargin=*/true, /*pageCountApproximate=*/chapter.approximate, syncIndicator,
                    /*lowMemory=*/memoryDegradedPage(currentSpineIndex, section->currentPage));

#if DEBUG_BACKGROUND_WORK
  renderBackgroundDebugOverlay();
#endif

  lastStatusBarPage = currentPage;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

std::string EpubReaderActivity::printedPageLabelFor(const uint16_t page) const {
  if (const auto label = section->getPrintedPageLabelForPage(page)) {
    // Exact-match label (already parenthesised, may be "7/8" when multiple anchors collapse).
    return *label;
  }
  if (const auto nearest = section->getNearestPrintedPageLabelAtOrBefore(page)) {
    // No pagebreak on this device page: the last printed-page label passed within this section
    // still tells the reader which printed page they're on.
    return std::string("(") + *nearest + ")";
  }
  return {};
}

EpubProgressRecord::Shown EpubReaderActivity::shownProgress() const {
  const ChapterPageSpan::Display chapter = chapterPageDisplay();
  EpubProgressRecord::Shown shown;
  shown.chapterPage = static_cast<uint16_t>(std::clamp(chapter.page, 0, 0xFFFF));
  shown.chapterTotal = static_cast<uint16_t>(std::clamp(chapter.total, 0, 0xFFFF));
  shown.printedPage = printedPageLabelFor(static_cast<uint16_t>(section->currentPage));
  return shown;
}

ChapterPageSpan::Display EpubReaderActivity::chapterPageDisplay() const {
  const bool building = section->hasActiveBuild();
  const int pageCount = building ? section->estimatedTotalPages() : section->pageCount;
  if (chapterSpanSpine_ != currentSpineIndex) {
    refreshChapterSpan();
  }
  ChapterPageSpan::Display chapter = chapterSpan_.apply(section->currentPage + 1, pageCount);
  chapter.approximate = chapter.approximate || building;
  return chapter;
}

EpubReaderActivity::ChapterBounds EpubReaderActivity::resolveChapterBounds(const int toc) const {
  ChapterBounds bounds;
  bounds.toc = toc;
  bounds.first = bounds.last = currentSpineIndex;
  // Without a TOC that names at least a quarter of the files there is no chapter list to group
  // them by (and getTocIndexForSpineIndex may already be answering one chapter per file).
  if (toc < 0 || !epub->hasReliableToc()) return bounds;
  const auto entry = epub->getTocItem(toc);
  const int first = entry.spineIndex;
  // An anchored entry starts mid-file, and a file holding several entries shares its pages between
  // them. Both are the per-page TOC lookup's business (Section::getTocIndexForPage), not a run of
  // whole files, so the counter stays per spine there.
  if (first < 0 || first > currentSpineIndex || !entry.anchor.empty()) return bounds;
  if (toc + 1 < epub->getTocItemsCount() && epub->getTocItem(toc + 1).spineIndex == first) return bounds;

  // A file with no TOC entry of its own inherits the one before it (BookMetadataCache), so the
  // chapter is the file the entry points at plus the run of inheritors after it. Walking forward by
  // spine rather than to the next TOC entry's spine also survives a TOC out of reading order.
  const int spineCount = epub->getSpineItemsCount();
  int last = currentSpineIndex;
  while (last + 1 < spineCount && epub->getTocIndexForSpineIndex(last + 1) == toc) {
    ++last;
    if (last - first + 1 > MAX_CHAPTER_SPAN_FILES) return bounds;
  }
  if (last - first + 1 > MAX_CHAPTER_SPAN_FILES) return bounds;
  bounds.first = first;
  bounds.last = last;
  return bounds;
}

void EpubReaderActivity::refreshChapterSpan() const {
  chapterSpanSpine_ = currentSpineIndex;
  chapterSpan_ = {};
  const int toc = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (toc != chapterBounds_.toc) {
    chapterBounds_ = resolveChapterBounds(toc);
  }
  const int first = chapterBounds_.first;
  const int last = chapterBounds_.last;
  if (first >= last || currentSpineIndex < first || currentSpineIndex > last) return;

  // Nothing per spine is held here (books with 1700+ spine items exist): the siblings' page counts
  // come from the book's SpinePageIndex on SD in one open and one sequential read, and their byte
  // sizes from four lookups in the cumulative spine table, whatever the chapter's length.
  const SpinePageIndex::Totals indexed = Section::indexedPageTotals(
      epub->getCachePath(), makeSectionBuildParams(), epub->getSpineItemsCount(), first, last, currentSpineIndex);
  const size_t chapterStart = first > 0 ? epub->getCumulativeSpineItemSize(first - 1) : 0;
  const size_t currentStart = currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t currentEnd = epub->getCumulativeSpineItemSize(currentSpineIndex);
  const size_t chapterEnd = epub->getCumulativeSpineItemSize(last);
  const auto bytesBefore = static_cast<uint32_t>(currentStart - chapterStart);
  const auto bytesAfter = static_cast<uint32_t>(chapterEnd - currentEnd);

  ChapterPageSpan span;
  span.pagesBefore = indexed.pagesBefore;
  span.pagesAfter = indexed.pagesAfter;
  span.knownBytes = indexed.bytesBefore + indexed.bytesAfter;
  span.currentBytes = static_cast<uint32_t>(currentEnd - currentStart);
  span.unknownBefore = static_cast<uint16_t>(currentSpineIndex - first - indexed.filesBefore);
  span.unknownAfter = static_cast<uint16_t>(last - currentSpineIndex - indexed.filesAfter);
  span.unknownBytesBefore = bytesBefore - std::min(bytesBefore, indexed.bytesBefore);
  span.unknownBytesAfter = bytesAfter - std::min(bytesAfter, indexed.bytesAfter);
  chapterSpan_ = span;
  LOG_DBG("ERS", "Chapter span: toc=%d spines %d-%d, recorded %u+%u pages, %u+%u files estimated", toc, first, last,
          static_cast<unsigned>(span.pagesBefore), static_cast<unsigned>(span.pagesAfter),
          static_cast<unsigned>(span.unknownBefore), static_cast<unsigned>(span.unknownAfter));
}

void EpubReaderActivity::renderBackgroundDebugOverlay() const {
#if DEBUG_BACKGROUND_WORK && DEBUG_BACKGROUND_OVERLAY
  // Background A state is latched per displayed page into backgroundAGlyph_ (see the
  // field comment), set just before renderStatusBar() draws it. The live scheduling
  // flags are cleared at the top of render() before this draws, so they always read
  // idle; latching at display time gives a glyph that is correct and visible:
  //   'x' hit  — this page was served from the Background-A pre-render buffer
  //   '-' miss — this page was rendered fresh (first page, heap-gated, or no pre-render)
  const char aGlyph = backgroundAGlyph_;

  // B is sampled from its state machine rather than the live percent alone: the overlay
  // only draws when a page renders, and B works precisely while the reader is idle — a
  // build usually finishes between two page turns, so a percent-only chip was almost
  // never visible (percent resets to -1 on completion). States:
  //   Bp      — probing whether the next section is already cached
  //   Bw      — waiting for the heap gates
  //   B<nn>%  — build in flight (percent of the chapter consumed)
  //   B+      — build complete, Section held for adoption on the next chapter cross
  //   B.      — settled with nothing held (already cached, refused, or failed)
  // Render task holds the render lock here and B mutates only under it, so the reads
  // are consistent.
  char bBuf[8];
  switch (backgroundBuildState_) {
    case BackgroundBuildState::Probe:
      snprintf(bBuf, sizeof(bBuf), "p");
      break;
    case BackgroundBuildState::WaitHeap:
      snprintf(bBuf, sizeof(bBuf), "w");
      break;
    case BackgroundBuildState::Building:
      snprintf(bBuf, sizeof(bBuf), "%d%%", backgroundBuildPercent_ >= 0 ? backgroundBuildPercent_ : 0);
      break;
    case BackgroundBuildState::Settled:
      snprintf(bBuf, sizeof(bBuf), "%s", backgroundSection_ ? "+" : ".");
      break;
  }

  // Build a compact "A<x|-> B<state>" string and draw it at the top-left of the content
  // area, over whatever the status bar drew. Intentionally crude — a diagnostic aid.
  char buf[24];
  snprintf(buf, sizeof(buf), "A%c B%s", aGlyph, bBuf);
  // Draw near the top-left corner; UI_10_FONT_ID is the small status font used elsewhere.
  renderer.drawText(UI_10_FONT_ID, 4, 2, buf, true, EpdFontFamily::BOLD);
#endif
}

// main.cpp polls skipLoopDelay() and preventAutoSleep() on the loop task, outside
// activityManager.loop() and with no render lock held. `section` belongs to whichever task holds
// the lock, and the render task replaces it: renderNormalPass's page-load recovery resets it, and
// buildSection assigns it. An unlocked `section->hasActiveBuild()` could therefore read a Section
// the render task had just freed. Diagnosis by Sung-jin Brian Hong in crosspoint-reader PR #3652,
// which fixed it there by reading the hint under a try-lock in main.cpp.
//
// Try, never block: a blocking lock would stall the loop, and with it input, behind a whole
// render pass. While a pass holds the lock the hints keep their last locked reading, because
// neither fixed answer is safe:
//  - preventAutoSleep() true counts every render pass as user activity, and the status-bar clock
//    renders once a minute, so the sleep timeout would never run out.
//  - skipLoopDelay() true would busy-yield through every render, build or no build. False would
//    sleep 10 ms per tick through renders during a build. That is upstream's scheduling change in
//    the same PR, and it needs its own device measurement here.
// The last reading is what the unlocked read used to return, minus the freed-Section case. It is
// stale for at most one locked stretch of a pass (renderContents drops the lock before its
// waveform wait), and staleness costs no build progress: the slices need this same lock, so none
// could run in that stretch anyway. The one long stretch is a Blocking (fallback) build, which runs
// whole inside a single pass while the hints still say "no current-section build". So that build
// no longer holds off auto-sleep, which matters only if it outlasts the sleep timeout (1 minute at
// the lowest setting). And the loop no longer busy-yields against it, unless Background-B was
// building when the pass began.
void EpubReaderActivity::refreshLoopHints() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) {
    return;
  }
  loopHintCurrentBuild_ = section && section->hasActiveBuild();
  loopHintBackgroundBuild_ = backgroundBuildState_ == BackgroundBuildState::Building;
}

bool EpubReaderActivity::preventAutoSleep() {
  refreshLoopHints();
  return loopHintCurrentBuild_;
}

bool EpubReaderActivity::skipLoopDelay() {
  refreshLoopHints();
  return loopHintCurrentBuild_ || loopHintBackgroundBuild_;
}

bool EpubReaderActivity::shouldSkipPeriodicUpdate() const {
  // Polled once a minute by ActivityManager::loop() on the loop task with no lock held, so it
  // had the same unlocked read of `section` as the hints above (and raced render() on
  // statusRefreshDeferred_). A pass holding the lock means "cannot tell": request the update. At
  // worst that is one redundant status-bar refresh, where skipping could leave the clock a minute
  // behind if the pass in flight drew its status bar before the minute turned. It does not take
  // B's borrow from a live build either: the pass in flight already returned it on entry
  // (recoverSecondaryBufferIfNeeded), unless build-through keeps it, and then the extra render
  // keeps it too.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) {
    return false;
  }
  // Background-B holds the secondary buffer: any render takes it back, and taking it back
  // discards B's live build (endBackgroundBorrow). A clock-minute or battery tick would do that
  // once a minute, and a build takes ~7-15 s of slices -- so a status refresh waits for B to hand
  // the buffer back (endBackgroundBorrow asks for it then). A page turn still renders at once.
  if (backgroundBorrowActive_) {
    statusRefreshDeferred_ = true;
    return true;
  }
  if (lastStatusBarPage < 0) return false;  // no baseline yet — let the first render happen
  const int currentPage = section ? section->currentPage + 1 : -1;
  if (currentPage != lastStatusBarPage) return false;
  if (SETTINGS.statusBarBattery) {
    if (static_cast<int>(powerManager.getBatteryPercentage()) != lastStatusBarBattery) return false;
  }
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    const int minute = now > 0 ? static_cast<int>(now / 60) : -1;
    if (minute != lastStatusBarClockMinute) return false;
  }
  return true;
}

namespace {
// Grown a little around the glyphs, and floored, because a footnote marker is often a single
// superscript digit a few pixels wide -- an exact box would be a target no finger can hit. Kept
// modest on purpose: these rects are tested BEFORE the page-turn zones, so an over-generous
// marker would eat page turns from the text around it.
constexpr int kLinkTargetPad = 6;
constexpr int kLinkTargetMin = 30;
}  // namespace

void EpubReaderActivity::publishPageLinkTargets(const Page& page, const int marginLeft, const int contentTop) const {
  TapTargets::Recorder::Builder targets;
  if (currentPageFootnotes.empty()) {
    TapTargets::readerLinks().record(targets);
    return;
  }

  const int fontId = getEffectiveReaderFontId();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // The links arrive in page order, so one forward walk resolves them all: whichever word first
  // completes the text of the link we are looking for IS that link. Matching in order rather than
  // by text alone is what keeps a marker "1" from being claimed by an unrelated "1" earlier in
  // the paragraph.
  size_t nextLink = 0;
  size_t markerPos = 0;  // how much of that link's text the run in progress has accounted for
  bool runOpen = false;
  int runLeft = 0, runTop = 0, runRight = 0, runBottom = 0;

  for (const auto& element : page.elements) {
    if (nextLink >= currentPageFootnotes.size()) break;
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    for (uint16_t i = 0; i < block->wordCount() && nextLink < currentPageFootnotes.size(); i++) {
      // A link no word can ever match -- an image marker, whose display text is empty, or text
      // that is nothing but brackets -- would otherwise sit at the head of the queue forever and
      // cost every link behind it its target. Step over it here; the footnote list off the reader
      // menu still reaches it.
      //
      // Anything else keeps its place: matching strictly in page order is what stops a marker "1"
      // being claimed by an unrelated "1" earlier in the paragraph, so a link whose text simply
      // is not on the page does still block the rest. That is the safe way to be wrong -- no
      // target beats a target on the wrong words.
      size_t markerCoreStart = 0;
      size_t markerCoreEnd = 0;
      while (nextLink < currentPageFootnotes.size() &&
             !LinkMarkerMatch::core(currentPageFootnotes[nextLink].number,
                                    strlen(currentPageFootnotes[nextLink].number), markerCoreStart, markerCoreEnd)) {
        nextLink++;
        markerPos = 0;
        runOpen = false;
      }
      if (nextLink >= currentPageFootnotes.size()) break;

      const char* marker = currentPageFootnotes[nextLink].number;
      const size_t markerLen = strlen(marker);
      const char* text = block->wordText(i);
      const size_t textLen = block->wordTextLen(i);

      bool matched = LinkMarkerMatch::consumeToken(marker, markerLen, markerPos, text, textLen);
      if (!matched && markerPos != 0) {
        // A multi-word run that started earlier and then diverged. Drop it and give this word a
        // fresh chance at being the first token, rather than losing the link to a false start.
        markerPos = 0;
        runOpen = false;
        matched = LinkMarkerMatch::consumeToken(marker, markerLen, markerPos, text, textLen);
      }
      if (!matched) continue;

      const TextBlock::WordBox box =
          block->wordBox(renderer, i, fontId, line->xPos + marginLeft, line->yPos + contentTop);
      if (box.width <= 0 || box.height <= 0) {
        markerPos = 0;
        runOpen = false;
        continue;
      }
      if (runOpen) {
        runLeft = std::min(runLeft, static_cast<int>(box.x));
        runTop = std::min(runTop, static_cast<int>(box.y));
        runRight = std::max(runRight, box.x + box.width);
        runBottom = std::max(runBottom, box.y + box.height);
      } else {
        runLeft = box.x;
        runTop = box.y;
        runRight = box.x + box.width;
        runBottom = box.y + box.height;
        runOpen = true;
      }

      if (!LinkMarkerMatch::complete(marker, markerLen, markerPos)) continue;

      int x = runLeft - kLinkTargetPad;
      int y = runTop - kLinkTargetPad;
      int w = std::max(runRight - runLeft + 2 * kLinkTargetPad, kLinkTargetMin);
      int h = std::max(runBottom - runTop + 2 * kLinkTargetPad, kLinkTargetMin);
      // Centre the floor on the glyphs rather than growing right/down only, and keep the rect on
      // the panel so a marker in a margin is still reachable.
      x -= std::max(0, (w - (runRight - runLeft + 2 * kLinkTargetPad)) / 2);
      y -= std::max(0, (h - (runBottom - runTop + 2 * kLinkTargetPad)) / 2);
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      if (x + w > screenW) w = screenW - x;
      if (y + h > screenH) h = screenH - y;
      if (w > 0 && h > 0) targets.add(x, y, w, h, static_cast<int>(nextLink));

      nextLink++;
      markerPos = 0;
      runOpen = false;
    }
  }
  TapTargets::readerLinks().record(targets);
}

// A tap on a footnote marker or cross-reference follows it, exactly as picking it out of the
// footnote list does -- navigateToHref with savePosition, so page-back returns to where the
// reader was.
//
// Single tap, not point-then-confirm. Two taps would need somewhere to show the highlight, which
// on a full page of text there is not; and the jump is the cheapest action in the reader to undo,
// because coming back is already a first-class gesture.
bool EpubReaderActivity::handleLinkTouch() {
#if !CP_TOUCH_UI
  // Unreachable without a digitiser, and cppcheck proves it: readerLinks().hitTest() is a
  // literal -1 in the stub, so it reads the guarded currentPageFootnotes[link] below as an
  // index of -1 and reports it at HIGH severity. The guard is correct -- the path is simply
  // dead -- so the honest answer is to not compile it.
  return false;
#else
  if (!mappedInput.hasTouch() || !SETTINGS.touchReaderControls) return false;
  if (!TapTargets::readerLinks().hasTargets()) return false;

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  const int link = TapTargets::readerLinks().hitTest(x, y);
  // Bounds-checked against the CURRENT list, not the one the render saw: the targets are
  // published on the render task and the page can turn under a tap.
  if (link < 0 || link >= static_cast<int>(currentPageFootnotes.size())) return false;

  mappedInput.suppressTouchContact();
  LOG_DBG("ERS", "Link tap at (%d,%d) -> %s", x, y, currentPageFootnotes[link].href);
  navigateToHref(currentPageFootnotes[link].href, true);
  return true;
#endif  // CP_TOUCH_UI
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  int sourceSpineIndex;
  {
    RenderLock lock(*this);
    sourceSpineIndex = currentSpineIndex;
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = sourceSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    return;
  }

  // The origin is pushed only once the jump is certain. Pushing first and popping it again on an
  // unresolvable href popped a DIFFERENT entry whenever the push itself had not happened (stack
  // full, or no section), and with a full stack now dropping its oldest entry the pop could not
  // bring that entry back anyway.
  {
    RenderLock lock(*this);
    const bool pushed = savePosition && section;
    if (pushed) {
      SavedPosition saved;
      saved.spineIndex = currentSpineIndex;
      saved.pageNumber = section->currentPage;
      // Mid-build pageCount is "pages so far", which would rescale the fallback against the wrong
      // total; the paragraph anchor below is unaffected either way.
      if (!section->hasActiveBuild()) {
        saved.pageCount = section->pageCount;
        if (const auto paragraphIndex = section->getParagraphIndexForPage(section->currentPage)) {
          saved.paragraphIndex = *paragraphIndex;
          saved.hasParagraph = true;
        }
      }
      EpubLinkBackStack::push(savedPositions, footnoteDepth, sessionLinkDepth_, saved);
      noteLinkBackStackChanged();
      LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d, paragraph %d", footnoteDepth, saved.spineIndex,
              saved.pageNumber, saved.hasParagraph ? saved.paragraphIndex : -1);
    }
    navTarget = anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(std::move(anchor));
    // Only a fragment can lead back to an origin: a note's return link names the marker's id.
    navTarget.followedLink = pushed && navTarget.kind == NavigationTarget::Kind::Anchor;
    currentSpineIndex = targetSpineIndex;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

// Following a note's "Return" link pushed the note as one more Back entry, so Back from the reader's
// own page went to the note again, and the KOSync push on sleep kept waiting for a return that had
// happened (sessionLinkDepth_ stayed above 0). Now a link that lands where an earlier one was
// followed from unwinds the stack to below that entry, the same entries Back would have popped.
// Checked where the page is known rather than when the link is tapped: a link into another spine
// has no page until that spine's section is open.
void EpubReaderActivity::settleFollowedLink(const int page) {
  if (!navTarget.followedLink) return;
  navTarget.followedLink = false;
  const int depth = EpubLinkBackStack::returnDepth(savedPositions, footnoteDepth, currentSpineIndex, page);
  if (depth < 0) return;
  LOG_DBG("ERS", "Link returned to saved position [%d] (spine %d, page %d); Back stack %d -> %d", depth,
          currentSpineIndex, page, footnoteDepth, depth);
  while (footnoteDepth > depth) EpubLinkBackStack::pop(footnoteDepth, sessionLinkDepth_);
  noteLinkBackStackChanged();
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  {
    // The pop and the file's removal happen together under the lock, as the push and its removal
    // do: a heap-recovery reboot saves the stack from the render task (maybeRestartForFragmentedHeap),
    // and between the two it could write back the entry just popped.
    RenderLock lock(*this);
    EpubLinkBackStack::pop(footnoteDepth, sessionLinkDepth_);
    noteLinkBackStackChanged();
    const auto& pos = savedPositions[footnoteDepth];
    LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d, paragraph %d", footnoteDepth, pos.spineIndex,
            pos.pageNumber, pos.hasParagraph ? pos.paragraphIndex : -1);
    currentSpineIndex = pos.spineIndex;
    navTarget = pos.hasParagraph ? NavigationTarget::makeParagraph(pos.paragraphIndex, pos.pageNumber)
                                 : NavigationTarget::makePage(pos.pageNumber);
    navTarget.cachedPageCount = pos.pageCount;
    navTarget.cachedSpineIdx = pos.spineIndex;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadLinkBackStack() {
  const std::string path = epub->getCachePath() + LINK_STACK_FILE;
  // Absent is the normal case; asking first keeps the open from logging a failed read for it.
  if (!Storage.exists(path.c_str())) return;
  // One byte more than the largest stack, so an over-long file reads as over-long, not as a stack.
  uint8_t data[EpubLinkBackStack::kMaxSize + 1];
  int size = 0;
  {
    FsFile f;
    if (!Storage.openFileForRead("ERS", path, f)) return;
    size = f.read(data, sizeof(data));
    f.close();
  }
  const int depth = EpubLinkBackStack::decode(data, size > 0 ? static_cast<size_t>(size) : 0,
                                              epub->getSpineItemsCount(), savedPositions);
  if (depth == 0) {
    // Torn, from another version, or for a spine the book no longer has: no Back stack at all is
    // the safe reading, and removing the file stops it being read again on every open.
    LOG_ERR("ERS", "Ignoring unusable link stack (%d bytes)", size);
    Storage.remove(path.c_str());
    return;
  }
  // sessionLinkDepth_ stays 0: none of these links was followed in this session.
  footnoteDepth = depth;
  linkStackOnDisk_ = true;
  LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
}

void EpubReaderActivity::saveLinkBackStack() {
  if (!epub || footnoteDepth <= 0 || linkStackOnDisk_) return;
  uint8_t data[EpubLinkBackStack::kMaxSize];
  const size_t size = EpubLinkBackStack::encode(savedPositions, footnoteDepth, data);
  FsFile f;
  if (!Storage.openFileForWrite("ERS", epub->getCachePath() + LINK_STACK_FILE, f)) {
    LOG_ERR("ERS", "Failed to open the link stack for writing");
    return;
  }
  // A short write leaves a file whose size does not match its depth, which the next open rejects.
  const bool written = f.write(data, size) == size;
  f.close();
  if (!written) {
    LOG_ERR("ERS", "Failed to write the link stack");
    return;
  }
  linkStackOnDisk_ = true;
  LOG_DBG("ERS", "Saved link stack, depth %d", footnoteDepth);
}

void EpubReaderActivity::noteLinkBackStackChanged() {
  if (!linkStackOnDisk_ || !epub) return;
  linkStackOnDisk_ = false;
  if (!Storage.remove((epub->getCachePath() + LINK_STACK_FILE).c_str())) {
    LOG_ERR("ERS", "Failed to remove the outdated link stack");
  }
}

bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  // Resolve per-book overrides FIRST: every layout input below must be the EFFECTIVE value
  // (global setting resolved against the book override), mirroring the reader's
  // getEffective*() helpers. Passing raw SETTINGS values here diverges the section property
  // hash from the reader's whenever an override is set, and this path then silently rebuilds
  // a duplicate section variant during sleep preparation.
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(filePath);
  const bool effectiveEmbeddedStyle =
      currentBook.embeddedStyleOverride >= 0 ? currentBook.embeddedStyleOverride != 0 : SETTINGS.embeddedStyle != 0;
  const bool effectiveHyphenation =
      currentBook.hyphenationOverride >= 0 ? currentBook.hyphenationOverride != 0 : SETTINGS.hyphenationEnabled != 0;
  const bool effectiveFontSizeNormalization = currentBook.fontSizeNormalizationOverride >= 0
                                                  ? currentBook.fontSizeNormalizationOverride != 0
                                                  : SETTINGS.fontSizeNormalization != 0;
  const bool effectiveBionicReading =
      currentBook.bionicReadingOverride >= 0 ? currentBook.bionicReadingOverride != 0 : SETTINGS.bionicReading != 0;
  const bool effectiveInlineFootnotePreviews = currentBook.inlineFootnotePreviewsOverride >= 0
                                                   ? currentBook.inlineFootnotePreviewsOverride != 0
                                                   : SETTINGS.inlineFootnotePreviews != 0;
  const uint8_t effectiveParagraphAlignment = currentBook.paragraphAlignmentOverride >= 0
                                                  ? static_cast<uint8_t>(currentBook.paragraphAlignmentOverride)
                                                  : SETTINGS.paragraphAlignment;
  const uint8_t effectiveImageRendering = currentBook.imageRenderingOverride >= 0
                                              ? static_cast<uint8_t>(currentBook.imageRenderingOverride)
                                              : SETTINGS.imageRendering;

  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is effectively enabled, as createSectionFile may need it to
  // rebuild the cache.
  if (!epub->load(true, !effectiveEmbeddedStyle)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) >= 4) {
      spineIndex = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      pageNumber = (int)((uint32_t)data[2] | ((uint32_t)data[3] << 8));
    }
    f.close();
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarTopHeight());
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  marginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarBottomHeight());

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const bool hasLocalSdOverride = !currentBook.sdFontFamilyOverride.empty();
  const uint8_t effectiveFontFamily =
      currentBook.fontFamilyOverride >= 0 ? static_cast<uint8_t>(currentBook.fontFamilyOverride) : SETTINGS.fontFamily;
  const uint8_t effectiveFontSize =
      currentBook.fontSizeOverride >= 0 ? static_cast<uint8_t>(currentBook.fontSizeOverride) : SETTINGS.fontPointSize;
  int effectiveFontId = 0;
  if (hasLocalSdOverride) {
    effectiveFontId = resolveSdCardFontId(currentBook.sdFontFamilyOverride.c_str(), effectiveFontSize);
  } else if (currentBook.fontFamilyOverride < 0) {
    // Publisher Fonts, as getEffectiveReaderFontId(): the book's embedded body family when it has one.
    effectiveFontId = resolveSdCardFontId(sdFontSystem.bookFamilyNameFor(epub->getCachePath()), effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontFamilyOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontIdForPoints(effectiveFontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    effectiveFontId = resolveSdCardFontId(SETTINGS.sdFontFamilyName, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontIdForPoints(SETTINGS.fontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0) {
    effectiveFontId = SETTINGS.getReaderFontId();
  }
  const auto getEffectiveLineCompression = [&](int fontId) {
    const int notosansId =
        CrossPointSettings::getBuiltinReaderFontIdForPoints(CrossPointSettings::NOTOSANS, effectiveFontSize);

    if (fontId == notosansId) {
      switch (SETTINGS.lineSpacing) {
        case CrossPointSettings::TIGHT:
          return 0.90f;
        case CrossPointSettings::NORMAL:
        default:
          return 0.95f;
        case CrossPointSettings::WIDE:
          return 1.0f;
      }
    }

    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.95f;
      case CrossPointSettings::NORMAL:
      default:
        return 1.0f;
      case CrossPointSettings::WIDE:
        return 1.1f;
    }
  };

  const float effectiveLineCompression = getEffectiveLineCompression(effectiveFontId);
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  Section::BuildParams p;
  p.fontId = effectiveFontId;
  p.lineCompression = effectiveLineCompression;
  p.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  p.paragraphAlignment = effectiveParagraphAlignment;
  p.viewportWidth = viewportWidth;
  p.viewportHeight = viewportHeight;
  p.hyphenationEnabled = effectiveHyphenation;
  p.fontSizeNormalization = effectiveFontSizeNormalization;
  p.embeddedStyle = effectiveEmbeddedStyle;
  p.bionicReadingEnabled = effectiveBionicReading;
  p.inlineFootnotePreviews = effectiveInlineFootnotePreviews;
  p.imageRendering = effectiveImageRendering;
  p.fontSizeLadder = buildReaderFontSizeLadder(effectiveFontId);
  // On the heap: about 0.5 KB with the book's accent families, too much for this frame under a section build.
  const auto fontFamilies = makeUniqueNoThrow<BlockFontFamilies>();
  if (fontFamilies) {
    buildBookFontFamilies(*fontFamilies, epub->getCachePath(), effectiveFontId, effectiveFontSize);
    p.fontFamilies = fontFamilies.get();
  }
  if (!section->loadSectionFile(p)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(p, nullptr, /*skipEviction=*/false)) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  // During an active build the on-disk LUT is not yet written; load from the in-memory LUT.
  auto page = section->hasActiveBuild() ? section->loadPageFromActiveBuild(static_cast<uint16_t>(pageNumber))
                                        : section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  const int imageOnlyOffset = getImageOnlyPageYOffset(*page, viewportHeight);
  const int renderMarginTop = marginTop + imageOnlyOffset;
  renderer.clearScreen();
  page->render(renderer, effectiveFontId, marginLeft, renderMarginTop);
  // No displayBuffer call — caller (SleepActivity) handles that after compositing the overlay
  return true;
}

void EpubReaderActivity::openQuickOverrides() {
  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<QuickOverridesActivity>(
          renderer, mappedInput, bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, bookBionicReadingOverride, bookGuideDotsOverride,
          bookParagraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
          bookFontSizeNormalizationOverride, bookInlineFootnotePreviewsOverride),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyBookReaderOverrides(
            menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride, menu.sdFontFamilyOverride,
            menu.fontSizeOverride, static_cast<int8_t>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
            menu.textAntiAliasingOverride, menu.hyphenationOverride, menu.fontSizeNormalizationOverride,
            menu.guideDotsOverride, menu.inlineFootnotePreviewsOverride);
      });
}

void EpubReaderActivity::openReaderMenu() {
  if (!epub) {
    return;
  }

  int currentPage = 0;
  int totalPages = 0;
  float bookProgress = 0.0f;
  bool isCurrentPageStarred = false;
  {
    RenderLock lock(*this);
    currentPage = section ? section->currentPage + 1 : 0;
    totalPages = section ? section->pageCount : 0;
    if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    }
    isCurrentPageStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                        static_cast<uint16_t>(section->currentPage));
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  // Show the "Go to printed page" item only when this book has at least one integer-labelled
  // entry in pagelist.bin. Roman-only or empty page lists are excluded — the numeric input
  // dialog can't address them anyway. Streamed (not loadPrintedPageList) so opening the menu never
  // reserves the whole list: that ~200 KB contiguous allocation aborts the firmware via uncaught
  // bad_alloc when the menu is opened mid section-build on a long book (heap fragmented to tens of
  // KB) — the tag 2.05 "Confirm reboots" crash.
  const bool hasPrintedPages = epub->hasNumericPrintedPages();

  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, SETTINGS.textDarkness, getEffectiveBionicReading(),
          bookGuideDotsOverride, bookParagraphAlignmentOverride, bookTextAntiAliasingOverride, bookHyphenationOverride,
          bookFontSizeNormalizationOverride, bookInlineFootnotePreviewsOverride, !bookmarkStore.isEmpty(),
          isCurrentPageStarred, hasPrintedPages),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        applyTextDarkness(menu.textDarkness);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyBookReaderOverrides(
            menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride, menu.sdFontFamilyOverride,
            menu.fontSizeOverride, static_cast<bool>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride,
            menu.textAntiAliasingOverride, menu.hyphenationOverride, menu.fontSizeNormalizationOverride,
            menu.guideDotsOverride, menu.inlineFootnotePreviewsOverride);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }

        // A deferred AA armed before the menu opened belongs to a frame that is no
        // longer on screen. Running it now would composite this page's text planes
        // over whatever the menu left behind: on LGFX panels displayGray() overlays
        // the live canvas rather than rebuilding from a base buffer, so a stale pass
        // corrupts the current image outright. Observed as an unreadable screen on
        // leaving the menu, with the AA landing ~14 s and nine menu refreshes after
        // the page it was computed for.
        {
          RenderLock lock(*this);
          pendingGrayscale_ = {};
        }

        // Arm a HALF for the resumed page, then repaint. Every other sub-activity handler
        // here already requests an update (book info, reading stats, chapter selection);
        // the menu's did not, so a plain Back left the menu on screen until something
        // else happened to trigger a render.
        //
        // The enforceExitFullRefresh() before the launch does NOT cover this: the override
        // is one-shot and the menu's own first paint consumes it on the way IN, which is
        // what it is there for. So the return repaint went out on the normal cycle, usually
        // FAST -- and a full-screen menu back to text is the worst case for a differential
        // bank, which cannot drive every changed pixel in one fast pass.
        //
        // This was left out on the argument that a HALF costs ~1.5 s and no ghosting had
        // been reported here. It has now: on the T5 S3 the transition shows the menu and the
        // page superimposed, then settles a refresh later. That is not even a saving -- the
        // reader was paying for a bad differential plus whatever repaired it. The chapter
        // selection handler below arms exactly this, for exactly this reason.
        ReaderUtils::enforceExitFullRefresh(renderer);
        requestUpdate();
      });
}

void EpubReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      pageTurn(true);
      break;
    case BA::BTN_PAGE_BACK:
      pageTurn(false);
      break;
    case BA::BTN_PAGE_FORWARD_10:
      jumpPages(true, 10);
      break;
    case BA::BTN_PAGE_BACK_10:
      jumpPages(false, 10);
      break;
    case BA::BTN_STAR_PAGE: {
      RenderLock lock(*this);
      if (section) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
    } break;
    case BA::BTN_DICTIONARY:
      openDictionary();
      break;
    case BA::BTN_FOOTNOTES:
      if (!currentPageFootnotes.empty()) {
        if (currentPageFootnotes.size() == 1) {
          navigateToHref(currentPageFootnotes[0].href, true);
        } else {
          auto linkInfo = pageLinkInfoForCurrentPage();
          startActivityForResult(
              std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes,
                                                            std::move(linkInfo.previews), std::move(linkInfo.isNote)),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                  navigateToHref(footnoteResult.href, true);
                }
              });
        }
      }
      break;
    case BA::BTN_OPEN_TOC:
      if (epub) {
        int spineIdx;
        int tocIdx;
        {
          RenderLock lock(*this);
          spineIdx = currentSpineIndex;
          tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                           : epub->getTocIndexForSpineIndex(currentSpineIndex);
        }
        ReaderUtils::enforceExitFullRefresh(renderer);
        startActivityForResult(std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub,
                                                                                    epub->getPath(), spineIdx, tocIdx),
                               [this](const ActivityResult& result) {
                                 if (result.isCancelled) return;
                                 // See the matching comment in onReaderMenuConfirm's SELECT_CHAPTER
                                 // case: the override armed before launch was already consumed by
                                 // the chapter list's own paint, so arm a fresh one for the resumed page.
                                 ReaderUtils::enforceExitFullRefresh(renderer);
                                 RenderLock lock(*this);
                                 const auto& chapter = std::get<ChapterResult>(result.data);
                                 auto resolvedPage =
                                     (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                         ? section->getPageForTocIndex(*chapter.tocIndex)
                                         : std::nullopt;
                                 if (resolvedPage) {
                                   section->currentPage = *resolvedPage;
                                   anchorNavTargetToCurrentPage();
                                   forceLoadLargeImages = false;
                                   pageHasPlaceholders = false;
                                 } else {
                                   navTarget = chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex)
                                                                : NavigationTarget::makePage(0);
                                   currentSpineIndex = chapter.spineIndex;
                                   section.reset();
                                 }
                               });
      }
      break;
    case BA::BTN_NEXT_SECTION:
    case BA::BTN_PREV_SECTION: {
      const bool forward = (action == BA::BTN_NEXT_SECTION);
      // Deliberate chapter jump: arm a HALF refresh for the next displayed screen, exactly like the
      // other deliberate jumps (percent / TOC / footnote / reader exit). The indexing popup consumes
      // this override so hasRefreshOverridePending() reads true there, which arms
      // forceHalfRefreshAfterPopup_; the first page of the target chapter then paints HALF instead of
      // a FAST differential. Without it, the dramatic previous-chapter -> new-chapter transition
      // under-drives on X4 and the previous chapter's text ghosts through the new page.
      ReaderUtils::enforceExitFullRefresh(renderer);
      {
        RenderLock lock(*this);
        if (section && section->pageCount > 0) {
          const int curTocIndex = section->getTocIndexForPage(section->currentPage);
          const int nextTocIndex = forward ? curTocIndex + 1 : curTocIndex - 1;
          if (curTocIndex < 0) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
            section.reset();
          } else if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
            const int newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);
            if (newSpineIndex == currentSpineIndex) {
              if (const auto resolvedPage = section->getPageForTocIndex(nextTocIndex)) {
                section->currentPage = *resolvedPage;
                anchorNavTargetToCurrentPage();
                forceLoadLargeImages = false;
                pageHasPlaceholders = false;
              }
            } else {
              navTarget = NavigationTarget::makeTocIndex(nextTocIndex);
              currentSpineIndex = newSpineIndex;
              section.reset();
            }
          } else if (forward) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getSpineItemsCount();
            section.reset();
          } else {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getTocItem(curTocIndex).spineIndex - 1;
            section.reset();
          }
        } else {
          navTarget = NavigationTarget::makePage(0);
          currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
      }
      requestUpdate();
      break;
    }
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      if (tryAutoPushOnClose()) break;
      finish();
      break;
    case BA::BTN_READER_MENU:
      if (epub) {
        openReaderMenu();
      }
      break;
    case BA::BTN_TOGGLE_BIONIC_READING:
      if (epub) {
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, bookFontSizeOverride, !getEffectiveBionicReading(),
                                 bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_CYCLE_FONT_SIZE:
      if (epub) {
        const uint8_t current =
            (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontPointSize;
        const ReaderSizeList sizes =
            sdFontSystem.sizeListFor(bookSdFontFamily(bookFontFamilyOverride, bookSdFontFamilyOverride));
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, static_cast<int8_t>(sizes.next(current)),
                                 bookBionicReadingOverride, bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_FONT_SIZE_SMALLER:
    case BA::BTN_FONT_SIZE_LARGER:
      if (epub) {
        const uint8_t current =
            (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontPointSize;
        const ReaderSizeList sizes =
            sdFontSystem.sizeListFor(bookSdFontFamily(bookFontFamilyOverride, bookSdFontFamilyOverride));
        const uint8_t next = sizes.step(current, action == BA::BTN_FONT_SIZE_LARGER ? 1 : -1);
        // Clamped at both ends, so at the limit this is a no-op -- don't pay for a repaginate and a
        // full repaint to show the same page again. Compared with the size SHOWN, not the one
        // stored: a stored size the family does not offer is drawn as its nearest, and "smaller"
        // from there can land on that same size.
        if (next != sizes.snap(current)) {
          applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                   bookSdFontFamilyOverride, static_cast<int8_t>(next), bookBionicReadingOverride,
                                   bookParagraphAlignmentOverride);
          requestUpdate();
        }
      }
      break;
    case BA::BTN_CYCLE_ORIENTATION:
    case BA::BTN_CYCLE_ORIENTATION_BACK:
      if (epub) {
        // + COUNT before the modulo so the reverse step stays positive: the
        // operands are promoted to int and (0 - 1) % 4 is -1, not 3.
        const int step = action == BA::BTN_CYCLE_ORIENTATION_BACK ? CrossPointSettings::ORIENTATION_COUNT - 1 : 1;
        const uint8_t nextOrientation =
            static_cast<uint8_t>((SETTINGS.orientation + step) % CrossPointSettings::ORIENTATION_COUNT);
        applyOrientation(nextOrientation);
        requestUpdate();
      }
      break;
    case BA::BTN_KOREADER_SYNC:
      launchKOReaderSync(SyncLaunchMode::COMPARE);
      break;
    case BA::BTN_QUICK_OVERRIDES:
      if (epub) {
        openQuickOverrides();
      }
      break;
    default:
      break;
  }
}

bool EpubReaderActivity::handleForcedRefresh(const HalDisplay::RefreshMode mode) {
  // Re-display the CURRENT page to clear ghosting — do NOT raw displayBuffer() (the
  // framebuffer may hold a Background-A pre-render of the *next* page, which would look
  // like a page turn). Clear the pre-render flags so classifyRenderPass() picks a Normal
  // render of the current page, request the forced mode for that render, and re-render.
  {
    RenderLock lock(*this);
    pendingPreRender = false;
    usePreRenderedBuffer = false;
    preRenderedPage.ready = false;
    preRenderedPlanesStaged_ = false;
    forceRefreshModeNextRender_ = static_cast<int8_t>(mode);
  }
  requestUpdate();
  return true;
}
