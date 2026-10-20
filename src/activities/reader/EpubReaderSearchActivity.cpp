#include "EpubReaderSearchActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::shared_ptr<Epub> epub, const int startSpineIndex,
                                                   std::string* queryStore)
    : Activity("EpubReaderSearch", renderer, mappedInput),
      epub(std::move(epub)),
      startSpineIndex(startSpineIndex),
      queryStore(queryStore),
      query(queryStore ? *queryStore : std::string()) {}

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  askForQuery();
}

void EpubReaderSearchActivity::cancelAndFinish() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::askForQuery() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH_BOOK), query, 64, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          cancelAndFinish();
          return;
        }
        startSearch(std::get<KeyboardResult>(result.data).text);
      });
}

void EpubReaderSearchActivity::startSearch(const std::string& text) {
  RenderLock lock(*this);
  query = text;
  if (queryStore) *queryStore = text;
  needle = booksearch::foldQuery(text);
  entries.clear();
  chapterTitles.clear();
  lastTitledSpine = -1;
  selectorIndex = 0;
  capped = false;
  spinesDone = 0;
  spinesSkipped = 0;
  spineCount = epub ? epub->getSpineItemsCount() : 0;
  if (startSpineIndex < 0 || startSpineIndex >= spineCount) startSpineIndex = 0;
  searching = !needle.empty() && spineCount > 0;
  lastRepaintMs = millis();
  requestUpdate();
}

void EpubReaderSearchActivity::searchNextChapter() {
  HalPowerManager::Lock powerLock;  // inflate + parse at full clock
  const int spine = (startSpineIndex + spinesDone) % spineCount;
  const auto href = epub->getSpineItem(spine).href;

  booksearch::ChapterSearcher searcher(needle, MAX_HITS - entries.size());
  // A press gives way at once: the loop handles it this pass, and the chapter is searched
  // again from its start on the next one.
  searcher.setAbortProbe([] { return gpio.hasPendingInput(); });
  const unsigned long t0 = millis();
  const bool read =
      !href.empty() && searcher.ok() && epub->readItemContentsToStream(href, searcher, 1024, searcher.stopFlag());
  searcher.finish();
  if (searcher.aborted()) return;
  // A capped search stops the stream on purpose; anything else that failed is reported.
  const bool failed = !read && !searcher.full();
  if (failed) LOG_ERR("SRCH", "Could not search spine %d", spine);
  LOG_DBG("SRCH", "Spine %d: %u hits in %lums", spine, static_cast<unsigned>(searcher.hits().size()), millis() - t0);

  {
    RenderLock lock(*this);
    for (auto& hit : searcher.takeHits()) {
      if (spine != lastTitledSpine) {
        const int toc = epub->getTocIndexForSpineIndex(spine);
        std::string title = toc >= 0 ? epub->getTocItem(toc).title : std::string();
        if (title.empty()) title = std::string(tr(STR_SECTION_PREFIX)) + std::to_string(spine + 1);
        chapterTitles.push_back(std::move(title));
        lastTitledSpine = spine;
      }
      entries.push_back(Entry{static_cast<uint16_t>(spine), static_cast<uint16_t>(chapterTitles.size() - 1),
                              hit.visibleOffset, hit.visibleEnd, std::move(hit.snippet)});
    }
    if (failed) spinesSkipped++;
    spinesDone++;
    if (entries.size() >= MAX_HITS) {
      capped = true;
      searching = false;
    } else if (spinesDone >= spineCount) {
      searching = false;
    }
  }

  const unsigned long now = millis();
  if (!searching || now - lastRepaintMs >= REPAINT_INTERVAL_MS) {
    lastRepaintMs = now;
    requestUpdate();
  }
}

void EpubReaderSearchActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) continue;
    if (ev.button == MappedInputManager::Button::Back) {
      if (searching) {
        RenderLock lock(*this);
        searching = false;  // keep what was found; Back again leaves
        requestUpdate();
        return;
      }
      cancelAndFinish();
      return;
    }
    if (ev.button == MappedInputManager::Button::Confirm && !entries.empty()) {
      const Entry& e = entries[selectorIndex];
      setResult(SearchResult{e.spineIndex, e.visibleOffset, e.visibleEnd});
      finish();
      return;
    }
  }

  const int total = static_cast<int>(entries.size());
  if (total > 0) {
    buttonNavigator.onNextList(selectorIndex, total, [this] { requestUpdate(); });
    buttonNavigator.onPreviousList(selectorIndex, total, [this] { requestUpdate(); });
  }

  if (searching) searchNextChapter();
}

std::string EpubReaderSearchActivity::headerText() const {
  char status[64];
  if (searching) {
    snprintf(status, sizeof(status), tr(STR_SEARCH_PROGRESS), spinesDone, spineCount,
             static_cast<unsigned>(entries.size()));
  } else if (capped) {
    snprintf(status, sizeof(status), tr(STR_SEARCH_FIRST_RESULTS), static_cast<unsigned>(entries.size()));
  } else {
    snprintf(status, sizeof(status), tr(STR_SEARCH_RESULTS), static_cast<unsigned>(entries.size()));
  }
  return "\xE2\x80\x9C" + query + "\xE2\x80\x9D  " + status;
}

void EpubReaderSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), headerText().c_str());

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (spinesSkipped > 0 && !searching) {
    char note[64];
    snprintf(note, sizeof(note), tr(STR_SEARCH_SKIPPED), spinesSkipped);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop, note);
    contentTop += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  }
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      searching ? tr(STR_SEARCHING) : tr(STR_SEARCH_NO_MATCHES));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight}, static_cast<int>(entries.size()),
        selectorIndex, [this](const int i) { return chapterTitles[entries[i].titleIndex]; },
        [this](const int i) { return entries[i].snippet; });
  }

  const bool hasItems = !entries.empty();
  const auto hints =
      mappedInput.mapHints(tr(STR_BACK), hasItems ? tr(STR_OPEN) : "", "", "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
  renderer.displayBuffer();
}

ListRowTap::Result EpubReaderSearchActivity::selectListRow(const int index) {
  return ListRowTap::apply(index, static_cast<int>(entries.size()), selectorIndex);
}

bool EpubReaderSearchActivity::pageList(ListPageDirection /*direction*/) { return true; }
