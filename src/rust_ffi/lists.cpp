// Lists and option popups: the two widgets the theme paints end to end.

#include <algorithm>
#include <string>
#include <vector>

#include "components/OptionPopup.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "internal.h"

using rust_ffi::asText;
using rust_ffi::asUiRect;

namespace {

// Collects the Rust-side option strings once, for the popup calls below.
std::vector<std::string> collectOptions(const char* (*optionText)(void*, int32_t), void* ctx, const int32_t count) {
  std::vector<std::string> options;
  if (!optionText || count <= 0) return options;

  options.reserve(static_cast<size_t>(count));
  for (int32_t i = 0; i < count; i++) {
    const char* text = optionText(ctx, i);
    options.emplace_back(text ? text : "");
  }
  return options;
}

}  // namespace

extern "C" {

void cpp_theme_draw_option_popup(const uint8_t* title, const char* (*optionText)(void*, int32_t), void* ctx,
                                 const int32_t count, const int32_t selected) {
  if (!g_rustRendererPtr || !optionText) return;
  const auto options = collectOptions(optionText, ctx, count);
  if (options.empty()) return;

  GUI.drawOptionPopup(*g_rustRendererPtr, title ? asText(title) : "", options, selected);
}

uint8_t cpp_option_popup_row_rect(const uint8_t* title, const char* (*optionText)(void*, int32_t), void* ctx,
                                  const int32_t count, const int32_t index, int32_t* outXywh) {
  if (!g_rustRendererPtr || !optionText || !outXywh || index < 0) return 0;
  const auto options = collectOptions(optionText, ctx, count);
  if (index >= static_cast<int32_t>(options.size())) return 0;

  // Reuses OptionPopup's own geometry so the dialog math is not duplicated.
  const auto layout = OptionPopup::computeLayout(*g_rustRendererPtr, title ? asText(title) : "", options);
  const auto& rect = layout.options[static_cast<size_t>(index)];

  outXywh[0] = rect.x;
  outXywh[1] = rect.y;
  outXywh[2] = rect.width;
  outXywh[3] = rect.height;
  return 1;
}

void cpp_theme_draw_scroll_indicator(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                                     const int32_t content, const int32_t visible, const int32_t offset) {
  if (!g_rustRendererPtr || content <= 0 || visible <= 0) return;

  // FreeInkUI's own indicator, the one the C++ list screens show. It takes
  // counts, but they are only ever used as proportions, so pixels work as well
  // as rows - and it draws nothing when everything already fits.
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(*g_rustRendererPtr, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const fui::ThemeTokens tokens = uiThemeTokens(ui.target);

  fui::drawListScrollIndicator(ui.target, asUiRect(x, y, width, height), static_cast<uint32_t>(content),
                               static_cast<uint32_t>(visible), static_cast<uint32_t>(offset < 0 ? 0 : offset),
                               tokens.listScrollWidth, tokens.listScrollSide, tokens.listScrollInset);
}

void cpp_theme_draw_list(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                         const int32_t itemCount, const int32_t selectedIndex,
                         const char* (*rowText)(void*, int32_t, int32_t), void* ctx) {
  if (!g_rustRendererPtr || !rowText || itemCount <= 0) return;

  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(*g_rustRendererPtr, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const fui::ThemeTokens tokens = uiThemeTokens(ui.target);

  // The row strings live in the Rust caller's own buffers for the whole call,
  // so the items point straight at them - no copy per row.
  std::vector<fui::ListItem> items;
  items.reserve(static_cast<size_t>(itemCount));
  for (int32_t i = 0; i < itemCount; i++) {
    fui::ListItem item;
    item.label = rowText(ctx, i, 0);
    item.subtitle = rowText(ctx, i, 1);
    item.value = rowText(ctx, i, 2);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  // Say the row geometry outright rather than letting Screen::list substitute
  // its own. cpui measured this band with these very numbers, and the component
  // draws only rows that fully fit - so a taller row here silently drops the
  // last one, and a one-row list (a Toggle) disappears entirely.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool anySubtitle =
      std::any_of(items.begin(), items.end(), [](const fui::ListItem& item) { return item.subtitle != nullptr; });
  props.rowHeight = static_cast<int16_t>(anySubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
  props.rowGap = static_cast<int16_t>(metrics.listRowGap);
  // cpui declared each row's touch region before rendering and routes taps
  // itself, so the component draws only and registers no competing hits.
  props.action = fui::NO_ACTION;
  props.valueInset = 8;  // air between the value and the row edge
  // Titles at the value's size so both sides of a row read as one unit, and
  // maxLines marks the style as set - an all-default smallText fails
  // textStyleUnset and Screen::list would substitute bodyText back.
  props.labelText = tokens.smallText;
  props.labelText.maxLines = 2;

  // Keep the selection on screen. There is no remembered scroll position here:
  // a draw is stateless, which is what BaseTheme::drawList did too.
  fui::Screen<1> screen(ui.frame, tokens);
  const fui::Rect safe = ui.frame.safeRect();
  const fui::Rect band = asUiRect(x, y, width, height);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(band.y - safe.y), static_cast<int16_t>(safe.right() - band.right()),
                  static_cast<int16_t>(safe.bottom() - band.bottom()), static_cast<int16_t>(band.x - safe.x)});
  if (selectedIndex >= 0) {
    const uint16_t visible = fui::listVisibleRows(screen.contentRect(), props.rowHeight, props.rowGap);
    props.topIndex = fui::listTopIndexFor(static_cast<int16_t>(selectedIndex), 0, visible ? visible : 1,
                                          static_cast<uint16_t>(itemCount));
  }

  // Screen::list() substitutes the theme's row height, gap, padding, selection
  // style and scroll indicator. Going through it rather than ui::list() keeps
  // that recipe in one place - the same one the C++ list screens use.
  screen.list(props);
}
}
