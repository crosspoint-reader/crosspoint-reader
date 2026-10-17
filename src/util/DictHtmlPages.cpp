#include "DictHtmlPages.h"

#include <Arduino.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "DictHtmlNormalize.h"

namespace {

// ENTRY gate: is there room to start a styled layout at all? Keeps enough for
// the parser's own state and the page/layout allocations that follow. Falling
// back to plain text is much cheaper than discovering the shortfall halfway
// through a layout.
constexpr size_t MIN_STYLED_FREE_HEAP = 40 * 1024;
constexpr size_t MIN_STYLED_MAX_ALLOC = 20 * 1024;

// RETAIN gate: is there still room to keep the pages coming? Checked per
// completed page, and deliberately far lower than the entry gate, because it
// measures a different heap: the parser is alive and holding its working set.
//
// Measured on device, a 2554-byte definition: free 50772 at entry, 29400 one
// page in -- the layout costs ~21 KB while it runs, all of it returned when the
// parser is destroyed. Reusing the entry number here charged that cost against
// the gate, so the styled path refused its own first page whenever entry heap
// was under ~61 KB, which in a reading session is always. Every definition
// silently fell back to plain text.
//
// The pages actually retained are bounded by the two count caps below; this is
// only here to catch genuine exhaustion, so it sits near the floor -- but not
// on it. It is only tested when a page completes, and a single page's layout
// was measured costing up to ~8 KB between two such checks (a 7 KB definition
// bottomed out at 7592 free while the floor was 12 KB). 16 KB keeps roughly
// that much in hand at the low point, and is still far below the entry heap of
// every definition that lays out successfully.
constexpr size_t MIN_STYLED_RETAIN_HEAP = 16 * 1024;
constexpr size_t MIN_STYLED_RETAIN_ALLOC = 8 * 1024;

// Bound the retained layout independently of the input size: compact markup can
// emit far more page elements than its byte count suggests, and every page here
// is held in RAM at once rather than streamed to a section file.
constexpr size_t MAX_STYLED_PAGES = 64;
constexpr size_t MAX_STYLED_PAGE_ELEMENTS = 512;

bool heapAllowsStyledLayout() {
  return ESP.getFreeHeap() >= MIN_STYLED_FREE_HEAP && ESP.getMaxAllocHeap() >= MIN_STYLED_MAX_ALLOC;
}

bool heapAllowsStyledRetain() {
  return ESP.getFreeHeap() >= MIN_STYLED_RETAIN_HEAP && ESP.getMaxAllocHeap() >= MIN_STYLED_RETAIN_ALLOC;
}

}  // namespace

bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, const int fontId,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              std::vector<std::unique_ptr<Page>>& pagesOut) {
  pagesOut.clear();
  if (definition.empty()) return false;
  if (!heapAllowsStyledLayout()) {
    LOG_ERR("DHTML", "Low heap for styled definition (%lu free, %lu max block)",
            static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return false;
  }

  // One fixed allocation (256 bytes on the C3). The pages must outlive the
  // parser, so they are moved into the caller's vector as they complete.
  pagesOut.reserve(MAX_STYLED_PAGES);
  // Paired with the per-page log below: the difference between them is what the
  // parser and the layout themselves cost, which is the number that decides
  // whether the retain floor is set against the right baseline.
  LOG_DBG("DHTML", "Styled layout starting (%u bytes, free=%lu contig=%lu)", static_cast<unsigned>(definition.size()),
          static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));

  bool resourceLimitHit = false;
  const char* limitReason = nullptr;
  size_t retainedElements = 0;
  bool parsed = false;
  {
    // Heap-allocated as Section does -- the parser object is far too large for a
    // stack local. A null epub is safe here because imageRendering=2 suppresses
    // <img> entirely, and every path that dereferences epub sits behind either
    // that check or a non-empty image src that only that path can set.
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        nullptr, renderer, fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing != 0,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled != 0,
        SETTINGS.fontSizeNormalization != 0, SETTINGS.bionicReading != 0,
        // Always true: past a limit the pages are dropped here, and the parse is left to finish.
        [&pagesOut, &resourceLimitHit, &retainedElements, &limitReason](std::unique_ptr<Page> page) {
          if (resourceLimitHit) return true;
          const size_t pageElements = page->elements.size();
          // Name the limit that fired. "Exceeded the budget" with three
          // possible causes is not a diagnosis, and the difference matters:
          // the two count caps mean the definition is genuinely too big to
          // hold, while the heap floor means only that this moment was a bad
          // one -- a distinction worth being able to read off a log.
          if (pagesOut.size() >= MAX_STYLED_PAGES) {
            limitReason = "page count";
          } else if (retainedElements + pageElements > MAX_STYLED_PAGE_ELEMENTS) {
            limitReason = "element count";
          } else if (!heapAllowsStyledRetain()) {
            limitReason = "free heap";
          }
          if (limitReason != nullptr) {
            LOG_ERR("DHTML", "Styled definition stopped on %s (pages=%u elements=%u free=%lu contig=%lu)", limitReason,
                    static_cast<unsigned>(pagesOut.size()), static_cast<unsigned>(retainedElements + pageElements),
                    static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
            resourceLimitHit = true;
            pagesOut.clear();
            return true;
          }
          retainedElements += pageElements;
          pagesOut.push_back(std::move(page));
          return true;
        },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2);
    if (!parser) {
      LOG_ERR("DHTML", "OOM: ChapterHtmlSlimParser");
      pagesOut.clear();
      return false;
    }

    // The size only drives the parser's progress reporting and its page-LUT
    // reserve; no progress callback is installed here, and the normalized form
    // is within a few percent of the source, so the definition's own length is
    // the right estimate.
    if (!parser->setup(definition.size())) {
      pagesOut.clear();
      return false;
    }

    // Normalize straight into the parser. A rejected write means the XML parser
    // has already failed, so this stops at the first bad byte rather than
    // pushing the rest of the fragment through a dead parser.
    const bool streamed = normalizeDictionaryHtml(definition, *parser);
    // finalize() regardless: it is what flushes the last page and tears the SAX
    // state down, and it reports a document that ended mid-element.
    parsed = parser->finalize() && streamed && parser->streamSucceeded();
    if (!parsed) LOG_ERR("DHTML", "Definition is not parseable as XHTML; falling back to plain text");
  }

  if (!parsed || resourceLimitHit || pagesOut.empty()) {
    pagesOut.clear();
    return false;
  }
  return true;
}
