#include "DictionaryFontSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void DictionaryFontSelectActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);
  sdFontSystem.refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();
  if (families.empty()) return;

  // Snapshot names so rediscovering SD fonts cannot invalidate the row labels.
  fontNames = makeUniqueNoThrow<FontName[]>(families.size());
  rowItems = makeUniqueNoThrow<fui::ListItem[]>(families.size());
  if (!fontNames || !rowItems) {
    LOG_ERR("SET", "OOM: dictionary font list");
    fontNames.reset();
    rowItems.reset();
    return;
  }
  for (const auto& family : families) {
    if (family.name.size() >= sizeof(FontName::value)) continue;
    strcpy(fontNames[fontCount].value, family.name.c_str());
    auto& item = rowItems[fontCount];
    item.label = fontNames[fontCount].value;
    item.actionValue = static_cast<int16_t>(fontCount);
    if (SETTINGS.dictionaryFontFamily == CrossPointSettings::DICTIONARY_FONT_SD &&
        family.name == SETTINGS.dictionarySdFontFamilyName) {
      item.value = tr(STR_SELECTED);
      nav.selected = fontCount;
    }
    ++fontCount;
  }
}

const char* DictionaryFontSelectActivity::headerTitle() const { return tr(STR_SD_FONT); }

void DictionaryFontSelectActivity::activateIndex(const int index) {
  if (index < 0 || index >= fontCount) return;
  app.clearTapFlash();
  const char* name = fontNames[index].value;
  if (SETTINGS.dictionaryFontFamily != CrossPointSettings::DICTIONARY_FONT_SD ||
      strcmp(SETTINGS.dictionarySdFontFamilyName, name) != 0) {
    strcpy(SETTINGS.dictionarySdFontFamilyName, name);
    SETTINGS.dictionaryFontFamily = CrossPointSettings::DICTIONARY_FONT_SD;
    SETTINGS.saveToFile();
  }
  finish();
}

void DictionaryFontSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListItem emptyItem;
  emptyItem.label = tr(STR_NO_FONTS_AVAILABLE);
  fui::ListProps props;
  props.items = fontCount ? rowItems.get() : &emptyItem;
  props.count = fontCount ? static_cast<uint16_t>(fontCount) : 1;
  props.action = ACTION_ROW;
  props.inputMask = fontCount ? fui::InputTouch : 0;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
