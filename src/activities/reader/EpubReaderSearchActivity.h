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
                           const char* query, int startSpineIndex, int startPage, uint16_t viewportWidth,
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
  int currentSpineIndex;
  int currentPage;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  SearchState state = SearchState::Searching;
  bool sectionLoaded = false;
  bool wrapped = false;

  bool preparePage();
  bool loadCurrentSection();
  bool reachedWrappedStop() const;
  void advanceSpine();
  void scanNextPage();
  void setFailure(SearchState failureState);
  void cancel();
};
