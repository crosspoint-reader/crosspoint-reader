#pragma once

#include <Epub.h>
#include <Epub/Section.h>

#include <array>
#include <cstdint>
#include <memory>

#include "activities/Activity.h"

class EpubReaderSearchActivity final : public Activity {
 public:
  // The book-wide scan route, owned as one concept. The scan starts at
  // (startSpineIndex, startPage), runs forward to the end of the book, wraps
  // once, and stops before re-examining stopPage (the page the search was
  // initiated from). A fresh search may scan stopPage once more only to finish
  // a partial match that began on its preceding page. make() encodes the one
  // invariant tying the route together:
  // a repeated "find next" begins one page past stopPage so it cannot re-return
  // the originating page, while a fresh search begins exactly at it.
  struct SearchRoute {
    int startSpineIndex;
    int startPage;
    int stopPage;

    static SearchRoute make(int spineIndex, int initiatedFromPage, bool findNext) {
      return SearchRoute{spineIndex, initiatedFromPage + (findNext ? 1 : 0), initiatedFromPage};
    }
  };

  EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub,
                           const char* query, const SearchRoute& route, uint16_t viewportWidth,
                           uint16_t viewportHeight);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override;

 private:
  enum class SearchState : uint8_t { Searching, NotFound, Error };

  std::shared_ptr<Epub> epub;
  Section section;
  std::array<char, Section::MAX_SEARCH_QUERY_BYTES + 1> query{};
  // `query` compiled once in the constructor (normalized pattern + KMP table)
  // and reused for every page scan instead of being rebuilt per page.
  Section::CompiledSearchQuery compiledQuery{};
  const SearchRoute route;
  int currentSpineIndex;
  int currentPage;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  SearchState state = SearchState::Searching;
  bool sectionLoaded = false;
  bool wrapped = false;
  // KMP partial-match length carried across consecutive pages of the same spine
  // so a query split across a page boundary (line-hyphenated word, or a phrase)
  // still matches. Reset at every reading-order discontinuity: scan start (0
  // init), spine change (advanceSpine), and the wrap.
  size_t scanMatched = 0;
  // Last progress percentage painted to the panel. Repaints are gated on this
  // changing so the e-ink panel is not refreshed per page. Starts at 0 because
  // onEnter() paints the initial 0% screen before the scan begins.
  int lastProgressPercent = 0;
  // Byte-weighted book position (0-1, via Epub::calculateProgress) where the
  // scan began, and the length of its route to the stop page (forward to the end
  // of the book, wrap, then up to the stop page). Captured once the start spine
  // loads; progress is (work since start) / route length. scanStartPos is
  // negative until captured.
  float scanStartPos = -1.0f;
  float scanRouteLength = 0.0f;

  bool preparePage();
  bool loadCurrentSection();
  bool reachedWrappedStop() const;
  bool shouldScanWrappedStopContinuation() const;
  void advanceSpine();
  void scanNextPage();
  // Approximate 0-100 fraction of the scan route completed: the byte-weighted
  // distance travelled since the search began, over the route length (forward to
  // the end of the book, wrap, then up to the originating page).
  int searchProgressPercent() const;
  void setFailure(SearchState failureState);
  void cancel();
};
