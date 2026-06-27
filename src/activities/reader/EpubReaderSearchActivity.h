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
  // KMP failure function for `query`, built once in the constructor and reused
  // for every page scan instead of being rebuilt on each pageContainsText().
  std::array<uint8_t, Section::MAX_SEARCH_QUERY_BYTES> queryPrefix{};
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
  // Fraction (0-1) of the start spine that precedes startPage, captured from the
  // start spine's own page count the first time it loads. Progress subtracts
  // this so a scan beginning mid-spine measures work done since it started, not
  // absolute book position. Negative until captured.
  float startPageFraction = -1.0f;

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
