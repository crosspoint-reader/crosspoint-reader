// Theme metrics and the chrome the theme draws.

#include <I18n.h>
#include <MappedInputManager.h>

#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "internal.h"

using rust_ffi::asText;

namespace {

// Mirrors ThemeMetric on the Rust side.
enum class Metric : uint8_t {
  TopPadding = 0,
  HeaderHeight = 1,
  VerticalSpacing = 2,
  ButtonHintsHeight = 3,
  ContentSidePadding = 4,
  ContentTop = 5,
  ContentBottom = 6,
  ListRowHeight = 7,
  ListRowHeightWithSubtitle = 8,
  ProgressBarHeight = 9,
  MinTouchSize = 10,
};

// A hint slot: null means "the firmware's standard label", empty means unused.
const char* hintLabel(const uint8_t* label, const char* standard) { return label ? asText(label) : standard; }

}  // namespace

extern "C" {

int32_t cpp_theme_metric(const uint8_t metric) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  switch (static_cast<Metric>(metric)) {
    case Metric::TopPadding:
      return metrics.topPadding;
    case Metric::HeaderHeight:
      return metrics.headerHeight;
    case Metric::VerticalSpacing:
      return metrics.verticalSpacing;
    case Metric::ButtonHintsHeight:
      return metrics.buttonHintsHeight;
    case Metric::ContentSidePadding:
      return metrics.contentSidePadding;
    case Metric::ContentTop:
      return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    case Metric::ContentBottom:
      return g_rustRendererPtr ? g_rustRendererPtr->getScreenHeight() - metrics.buttonHintsHeight : 0;
    case Metric::ListRowHeight:
      return metrics.listRowHeight;
    case Metric::ListRowHeightWithSubtitle:
      return metrics.listWithSubtitleRowHeight;
    case Metric::ProgressBarHeight:
      return metrics.progressBarHeight;
    case Metric::MinTouchSize:
      // Scales with the body line height, which only the font-bound target
      // knows. Reached on touch frames only.
      if (!g_rustRendererPtr) return 0;
      return uiThemeTokens(makeUiTarget(*g_rustRendererPtr)).minTouchSize;
  }
  return 0;
}

void cpp_theme_draw_header(const uint8_t* title, const uint8_t* subtitle) {
  if (!g_rustRendererPtr) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = g_rustRendererPtr->getScreenWidth();

  GUI.drawHeader(*g_rustRendererPtr, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 title ? asText(title) : "", subtitle ? asText(subtitle) : nullptr);
}

void cpp_theme_draw_sub_header(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                               const uint8_t* label, const uint8_t* rightLabel) {
  if (!g_rustRendererPtr || !label) return;
  GUI.drawSubHeader(*g_rustRendererPtr, Rect{x, y, width, height}, asText(label),
                    rightLabel ? asText(rightLabel) : nullptr);
}

void cpp_theme_draw_button_hints(const uint8_t* back, const uint8_t* confirm, const uint8_t* previous,
                                 const uint8_t* next) {
  if (!g_rustRendererPtr || !g_rustInputPtr) return;

  // mapLabels reorders these for the user's front-button layout, so Rust passes
  // them by meaning and never by physical slot.
  const auto labels = g_rustInputPtr->mapLabels(hintLabel(back, tr(STR_BACK)), hintLabel(confirm, tr(STR_SELECT)),
                                                hintLabel(previous, tr(STR_DIR_UP)), hintLabel(next, tr(STR_DIR_DOWN)));

  GUI.drawButtonHints(*g_rustRendererPtr, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void cpp_theme_draw_progress_bar(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                                 const uint32_t current, const uint32_t total) {
  if (!g_rustRendererPtr) return;
  GUI.drawProgressBar(*g_rustRendererPtr, Rect{x, y, width, height}, current, total);
}
}
