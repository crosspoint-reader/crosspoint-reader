#pragma once

#include <Epub.h>
#include <Epub/Section.h>

#include <array>
#include <cstdint>
#include <memory>

#include "activities/Activity.h"

class EpubReaderSearchActivity final : public Activity {
 public:
  EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub,
                           const char* query, int startSpineIndex, int startPage, int stopPage, uint16_t viewportWidth,
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
  const int startSpineIndex;
  const int startPage;
  // Page in startSpineIndex at which a wrapped scan stops, i.e. the position the
  // search was initiated from. Distinct from startPage: for a repeated "find
  // next" the scan starts at startPage (previous match + 1) but must still stop
  // before re-examining stopPage (the previous match), so it cannot re-return it.
  const int stopPage;
  int currentSpineIndex;
  int currentPage;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  SearchState state = SearchState::Searching;
  bool sectionLoaded = false;
  bool wrapped = false;
  // Last spine-derived progress percentage painted to the panel. Repaints are
  // gated on this changing so the e-ink panel is not refreshed per page. Starts
  // at 0 because onEnter() paints the initial 0% screen before the scan begins.
  int lastProgressPercent = 0;
  // The start spine's own page count, captured the first time it loads, used to
  // place startPage and stopPage as fractions within that spine. Progress
  // subtracts the startPage fraction (so a scan beginning mid-spine measures
  // work done since it started) and normalizes by the actual route length (so a
  // find-next that excludes the originating page can still reach 100%). Negative
  // until captured.
  int startSpinePageCount = -1;

  bool preparePage();
  bool loadCurrentSection();
  bool reachedWrappedStop() const;
  void advanceSpine();
  void scanNextPage();
  // Approximate 0-100 progress through the book, measured in spines scanned
  // from the search start position across the single wrap.
  int searchProgressPercent() const;
  void setFailure(SearchState failureState);
  void cancel();
};
