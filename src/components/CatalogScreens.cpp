#include "CatalogScreens.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "UIScale.h"
#include "UITheme.h"

namespace fui = freeink::ui;

void catalogScreenHeader(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const char* title,
                         const fui::BitmapRef trailingIcon, const fui::ActionId trailingAction) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  // Same top offset as every GUI.drawHeader caller, so the band lines up with
  // the rest of the firmware's screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().topPadding));
  fui::HeaderProps header;
  header.title = title;
  header.borderEdges = fui::EdgeBottom;
  if (trailingIcon && trailingAction != fui::NO_ACTION) {
    header.trailingIcon = trailingIcon;
    header.trailingAction = trailingAction;
    // Optically align the icon with the title glyphs: text hangs low in its
    // line cell by the font's internal leading; drop the button to match.
    const int titleFontId = uiScaleSpec().titleFontId;
    header.actionOffsetY =
        static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  }
  screen.header(header);
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void catalogCenteredBlock(UiAppHost::UiScreen& screen, const std::initializer_list<CatalogLine> lines) {
  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = screen.theme().spaceMd;
  const int count = static_cast<int>(lines.size());
  if (count == 0) return;
  const int16_t blockH = static_cast<int16_t>(lh * count + gap * (count - 1));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
  int i = 0;
  for (const CatalogLine& line : lines) {
    fui::TextStyle style = centered;
    style.bold = line.bold;
    screen.target().text(screen.takeTop(lh, ++i < count ? gap : 0), line.text ? line.text : "", style);
  }
}

void catalogDownloadScreen(UiAppHost::UiScreen& screen, const char* status, const size_t progress, const size_t total,
                           const fui::ActionId cancelAction) {
  // Centered block: status line, item title, progress, optional cancel.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const bool showBytes = total == 0;
  const int16_t progressH = showBytes ? lh : 16;
  const bool withCancel = cancelAction != fui::NO_ACTION;
  const int16_t btnH = withCancel ? theme.rowHeight : 0;
  // The item title gets two padded lines: long book titles wrap once and then
  // truncate, instead of running edge-to-edge in a single clipped line.
  fui::TextStyle title = centered;
  title.maxLines = 2;
  const int16_t titleH = static_cast<int16_t>(lh * 2);
  const int16_t pad = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  const int16_t blockH = static_cast<int16_t>(lh + titleH + progressH + btnH + gap * (withCancel ? 3 : 2));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(titleH, gap).inset(fui::Insets{0, pad, 0, pad}), status, title);

  const fui::Rect progressArea = screen.takeTop(progressH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (!showBytes) {
    fui::ProgressBarProps progressProps;
    progressProps.value = static_cast<int32_t>(progress);
    progressProps.max = static_cast<int32_t>(total);
    progressProps.border = fui::Paint::solid(fui::Color::Black);
    progressProps.borderWidth = 1;
    fui::progressBar(screen.frame(), progressArea, progressProps);
  } else {
    char bytes[24];
    if (progress >= 1024 * 1024) {
      snprintf(bytes, sizeof(bytes), "%u.%u MB", static_cast<unsigned>(progress >> 20),
               static_cast<unsigned>((progress >> 10) % 1024 * 10 / 1024));
    } else {
      snprintf(bytes, sizeof(bytes), "%u KB", static_cast<unsigned>(progress >> 10));
    }
    screen.target().text(progressArea, bytes, centered);
  }

  if (withCancel) {
    const fui::Rect btnArea = screen.takeTop(btnH);
    const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
    fui::ButtonProps cancel;
    cancel.label = tr(STR_CANCEL);
    cancel.action = cancelAction;
    screen.button(cancel,
                  fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
  }
}

void catalogListBody(UiAppHost::UiScreen& screen, const MappedInputManager& input, fui::ListNav& nav,
                     fui::ListProps& props, const int count, const bool hasSubtitle) {
  int16_t rowHeight = screen.theme().rowHeight;
  if (!input.hasTouch()) {
    // Non-touch hardware (X3/X4) keeps the original, denser per-theme row
    // height instead of FreeInkUI's touch-target-sized default (see
    // UiListActivity::syncListViewport; these screens sync their own viewport).
    const auto& metrics = UITheme::getInstance().getMetrics();
    rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
    props.rowHeight = rowHeight;
  }
  nav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, count, props);
  screen.list(props);
}
