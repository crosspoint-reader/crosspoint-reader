#include "EpubSearchResultsActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int SUBHEADER_HEIGHT = 26;
}  // namespace

EpubSearchResultsActivity::EpubSearchResultsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string query, std::vector<EpubSearchResult> results)
    : UiListActivity("EpubSearchResults", renderer, mappedInput),
      query(std::move(query)),
      results(std::move(results)) {
  buildRowItems();
}

void EpubSearchResultsActivity::onEnter() {
  RenderLock lock;
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  UiListActivity::onEnter();
}

void EpubSearchResultsActivity::buildRowItems() {
  rowItems.clear();
  rowItems.reserve(results.size());
  formattedSnippets.clear();
  formattedSnippets.reserve(results.size());

  for (size_t i = 0; i < results.size(); ++i) {
    auto& res = results[i];
    if (res.chapterTitle.empty()) {
      res.chapterTitle = std::to_string(i + 1) + ".";
    }

    std::string snippet;
    snippet.reserve(res.preContext.size() + res.match.size() + res.postContext.size() + 8);
    if (!res.preContext.empty()) {
      snippet += tr(STR_ELLIPSIS);
      snippet += res.preContext;
    }
    snippet += res.match;
    if (!res.postContext.empty()) {
      snippet += res.postContext;
      snippet += tr(STR_ELLIPSIS);
    }
    formattedSnippets.push_back(std::move(snippet));
    std::string().swap(res.preContext);
    std::string().swap(res.match);
    std::string().swap(res.postContext);

    fui::ListItem item;
    item.label = res.chapterTitle.c_str();
    item.subtitle = formattedSnippets.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void EpubSearchResultsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();

  ProgressChangeResult change;
  change.spineIndex = results[index].spineIndex;
  change.hasVisibleTextOffset = true;
  change.visibleTextOffset = results[index].visibleTextOffset;
  setResult(change);
  finish();
}

void EpubSearchResultsActivity::onBackButton() {
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

void EpubSearchResultsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + SUBHEADER_HEIGHT),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (results.empty()) {
    screen.centeredText(tr(STR_NO_MATCHES_FOUND), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubSearchResultsActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerY = safe.y + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{safe.x, headerY, safe.width, metrics.headerHeight}, tr(STR_SEARCH_RESULTS));

  const int subheaderY = headerY + metrics.headerHeight;

  // Right: "<N> matches"
  char matchesBuf[32];
  const char* matchUnit = (results.size() == 1) ? tr(STR_MATCH) : tr(STR_MATCHES);
  snprintf(matchesBuf, sizeof(matchesBuf), "%zu %s", results.size(), matchUnit);
  const int matchesWidth = renderer.getTextWidth(SMALL_FONT_ID, matchesBuf);
  renderer.drawText(SMALL_FONT_ID, safe.x + safe.width - matchesWidth - 8, subheaderY + 6, matchesBuf, true);

  // Left: Query: "<query>" (clamped so it never overlaps the match count)
  char queryBuf[96];
  snprintf(queryBuf, sizeof(queryBuf), "%s \"%s\"", tr(STR_QUERY), query.c_str());
  const int maxQueryWidth = std::max(0, safe.width - matchesWidth - 24);
  const std::string truncatedQuery = renderer.truncatedText(SMALL_FONT_ID, queryBuf, maxQueryWidth);
  renderer.drawText(SMALL_FONT_ID, safe.x + 8, subheaderY + 6, truncatedQuery.c_str(), true);

  // Subtle separator line below the subheader
  renderer.drawLine(safe.x, subheaderY + SUBHEADER_HEIGHT - 1, safe.x + safe.width - 1,
                    subheaderY + SUBHEADER_HEIGHT - 1);
}

void EpubSearchResultsActivity::drawFooter() {
  if (results.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  UiListActivity::drawFooter();
}
