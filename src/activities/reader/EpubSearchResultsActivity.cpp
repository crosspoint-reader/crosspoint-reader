#include "EpubSearchResultsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int SUBHEADER_HEIGHT = 26;
}  // namespace

EpubSearchResultsActivity::EpubSearchResultsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string query, const std::vector<EpubSearchResult>& results)
    : UiListActivity("EpubSearchResults", renderer, mappedInput), query(std::move(query)), results(results) {
  buildRowItems();
}

void EpubSearchResultsActivity::buildRowItems() {
  rowItems.clear();
  rowItems.reserve(results.size());
  rowTitles.clear();
  rowTitles.reserve(results.size());
  formattedSnippets.clear();
  formattedSnippets.reserve(results.size());

  for (size_t i = 0; i < results.size(); ++i) {
    const auto& res = results[i];
    std::string title = res.chapterTitle.empty() ? (std::to_string(i + 1) + ".") : res.chapterTitle;
    rowTitles.push_back(std::move(title));

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

    fui::ListItem item;
    item.label = rowTitles.back().c_str();
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

bool EpubSearchResultsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
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

  // Left: Query: "<query>"
  char queryBuf[64];
  snprintf(queryBuf, sizeof(queryBuf), "%s \"%s\"", tr(STR_QUERY), query.c_str());
  renderer.drawText(SMALL_FONT_ID, safe.x + 8, subheaderY + 6, queryBuf, true);

  // Right: "<N> matches"
  char matchesBuf[32];
  const char* matchUnit = (results.size() == 1) ? tr(STR_MATCH) : tr(STR_MATCHES);
  snprintf(matchesBuf, sizeof(matchesBuf), "%zu %s", results.size(), matchUnit);
  const int matchesWidth = renderer.getTextWidth(SMALL_FONT_ID, matchesBuf);
  renderer.drawText(SMALL_FONT_ID, safe.x + safe.width - matchesWidth - 8, subheaderY + 6, matchesBuf, true);

  // Subtle separator line below the subheader
  renderer.drawLine(safe.x, subheaderY + SUBHEADER_HEIGHT - 1, safe.x + safe.width - 1,
                    subheaderY + SUBHEADER_HEIGHT - 1);
}

void EpubSearchResultsActivity::drawFooter() {
  const auto labels = results.empty() ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                                      : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
