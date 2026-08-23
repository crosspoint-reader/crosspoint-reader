#include "ReaderToolbarUi.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/readerToolbarIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_DISMISS = 1;  // tap outside the sheet (page / top bar)
constexpr fui::ActionId ACTION_TOOL = 2;     // value = 0 Contents, 1 Text, 2 More
constexpr fui::ActionId ACTION_PREV = 3;     // scrub row: previous chapter
constexpr fui::ActionId ACTION_NEXT = 4;     // scrub row: next chapter
constexpr fui::ActionId ACTION_SCRUB = 5;    // progress track: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;      // panel list row, value = row index
constexpr fui::ActionId ACTION_HOME = 7;     // top bar back button: leave the book for the library

// Scrub row: two small round-cornered chapter buttons flanking a thin progress
// track with a round knob -- the reading page's chrome is light, so the
// controls stay slim rather than control-center sized.
constexpr int16_t kScrubButton = 36;  // chapter step buttons (square)
constexpr int16_t kScrubKnob = 16;    // round knob on the 2px progress track
constexpr int16_t kScrubGap = 12;     // air between the buttons and the track
// Tool row: a 24px glyph over a small label per slot, the active slot in an
// outline pill. The whole slot is the tap target.
constexpr int16_t kToolRowH = 64;
constexpr int16_t kToolPillInset = 10;
constexpr int16_t kToolIconTop = 8;
constexpr int kToolCount = 3;
// Top bar: at least this tall; the iOS-6 back tab's height, arrow depth and
// label padding.
constexpr int16_t kHeaderMinH = 60;
constexpr int16_t kBackButtonH = 32;
constexpr int16_t kBackArrow = 12;
constexpr int16_t kBackPadX = 10;
// Bottom sheet height for the panels, as a share of the screen: the page stays
// readable above it, and the list still gets several finger-sized rows.
constexpr int kPanelHeightPercent = 62;
}  // namespace

ReaderToolbarUi::ReaderToolbarUi(GfxRenderer& renderer) : UiAppHost(renderer) {}

void ReaderToolbarUi::begin() {
  resetUi();
  pending_ = Routed{};
  visibleRows_ = 0;
  topIndex_ = 0;
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_HOME; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() { renderUi(); }

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub track is a drag target, so held frames must reach it.
  const auto touch = routeTouch(input, false, /*routeHeld=*/true);
  pending_.routed = touch.routed;
  pending_.x = touch.snap.touchX;
  pending_.y = touch.snap.touchY;
  // Only the release commits a scrub: every held frame dispatches too
  // (dragPermille set), and re-paginating a chapter per frame would be seconds
  // of work per swipe.
  if (pending_.event == Event::Scrub && !touch.snap.touchReleased) pending_.event = Event::None;
  return pending_;
}

void ReaderToolbarUi::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  Routed& out = self->pending_;
  out.value = event.value;
  out.permille = event.dragPermille;
  switch (event.action) {
    case ACTION_DISMISS:
      out.event = Event::Dismiss;
      break;
    case ACTION_TOOL:
      out.event = Event::Tool;
      break;
    case ACTION_PREV:
      out.event = Event::PrevChapter;
      break;
    case ACTION_NEXT:
      out.event = Event::NextChapter;
      break;
    case ACTION_SCRUB:
      if (event.dragPermille >= 0) out.event = Event::Scrub;
      break;
    case ACTION_ROW:
      out.event = Event::Row;
      break;
    case ACTION_HOME:
      out.event = Event::Home;
      break;
    default:
      break;
  }
  // A handled action repaints through the reader's own fast path, not through
  // app.invalidate(): the page underneath is the reader's to draw.
  self->app.clearTapFlash();
}

void ReaderToolbarUi::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  if (self->model_.panel) {
    self->buildPanel(screen);
  } else {
    self->buildToolbar(screen);
  }
}

int16_t ReaderToolbarUi::toolRowHeight(const UiScreen& screen) const {
  (void)screen;
  return kToolRowH;
}

// The Contents / Text / More row: three equal slots, each a glyph over its
// label, the active one inside an outline pill (the theme's control radius).
// Each slot is registered as one tap target; the glyph and label are drawn
// straight into it, so the row stays light (no filled tiles).
void ReaderToolbarUi::buildToolRow(UiScreen& screen, const fui::LayoutAnchor anchor) {
  const auto& tokens = screen.theme();
  const char* labels[kToolCount] = {tr(STR_TOOL_CONTENTS), tr(STR_TOOL_TEXT), tr(STR_TOOL_MORE)};
  const fui::BitmapRef icons[kToolCount] = {fui::bitmapFromIcon(icon_reader_contents_24),
                                            fui::bitmapFromIcon(icon_reader_text_24),
                                            fui::bitmapFromIcon(icon_reader_more_24)};
  const fui::Rect row = screen.take(anchor, kToolRowH);
  const int16_t slotW = static_cast<int16_t>(row.width / kToolCount);
  const int16_t labelH = screen.target().lineHeight(tokens.smallText.font);
  fui::TextStyle labelStyle = tokens.smallText;
  labelStyle.align = fui::TextAlign::Center;
  const uint8_t pillRadius = static_cast<uint8_t>(std::min<int>(tokens.controlRadius, (kToolRowH - 2 * 4) / 2));
  for (int i = 0; i < kToolCount; ++i) {
    const fui::Rect slot{static_cast<int16_t>(row.x + slotW * i), row.y, slotW, row.height};
    if (i == model_.activeTool) {
      screen.target().stroke(slot.inset(fui::Insets{4, kToolPillInset, 4, kToolPillInset}),
                             fui::Paint::solid(fui::Color::Black), 2, pillRadius);
    }
    const fui::Rect iconRect{static_cast<int16_t>(slot.x + (slot.width - 24) / 2),
                             static_cast<int16_t>(slot.y + kToolIconTop), 24, 24};
    screen.target().bitmap(iconRect, icons[i], fui::BitmapMode::Center);
    const fui::Rect labelRect{slot.x, static_cast<int16_t>(slot.y + kToolIconTop + 24 + 2), slot.width, labelH};
    screen.target().text(labelRect, labels[i], labelStyle);
    screen.frame().hit(slot, ACTION_TOOL, static_cast<int16_t>(i), fui::InputTouch);
  }
}

// Top bar: a white band (at least kHeaderMinH tall -- the reading page's
// chrome needs a little more presence than a list header) with an iOS-6 style
// "back" button on the left (a bordered tab whose left end is an arrow) that
// leaves the book for the library, the book title centred, and the battery on
// the right. Detached-battery themes (Lyra) keep their own structure: battery
// in the top strip, title and button on the lower sub-band.
void ReaderToolbarUi::buildHeader(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect body = screen.body();
  const int16_t bandH = std::max<int16_t>(tokens.headerHeight, kHeaderMinH);
  const fui::Rect band = screen.takeTop(bandH);
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const fui::Paint paper = fui::Paint::solid(fui::Color::White);
  (void)body;

  // The page text runs under this band: paint it out, then the bottom rule.
  screen.target().fill(band, paper);
  if (tokens.headerUnderline > 0) {
    screen.target().fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - tokens.headerUnderline), band.width,
                                   static_cast<int16_t>(tokens.headerUnderline)},
                         ink);
  }

  // Battery (right).
  const uint16_t percent = powerManager.getBatteryPercentage();
  const bool showPercent = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  snprintf(batteryText_, sizeof(batteryText_), "%u%%", static_cast<unsigned>(percent));
  const int16_t labelW =
      showPercent ? screen.target().measureText(tokens.smallText.font, batteryText_, tokens.smallText).width : 0;
  constexpr int16_t kBatteryGap = 4;       // BaseTheme::batteryPercentSpacing
  constexpr int16_t kBatteryNubWidth = 2;  // the glyph's terminal nub past glyphWidth
  const int16_t batteryReserve =
      static_cast<int16_t>(metrics.batteryWidth + kBatteryNubWidth + (showPercent ? labelW + kBatteryGap : 0));
  const bool batteryDetached = metrics.headerBatteryDetached;
  fui::BatteryIndicatorProps battery;
  battery.percent = static_cast<uint8_t>(percent > 100 ? 100 : percent);
  battery.charging = gpio.isUsbConnected();
  battery.label = showPercent ? batteryText_ : nullptr;
  battery.text = tokens.smallText;
  battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  battery.gap = kBatteryGap;
  const int16_t batteryEdgeInset = batteryDetached ? 12 : tokens.headerSidePadding;
  const int16_t batteryX = static_cast<int16_t>(band.right() - batteryEdgeInset - batteryReserve);
  const int16_t batteryH = batteryDetached ? static_cast<int16_t>(metrics.batteryBarHeight) : bandH;
  fui::batteryIndicator(screen.frame(), fui::Rect{batteryX, band.y, batteryReserve, batteryH}, battery);

  // The line the title and the back button share: the whole band, or the
  // sub-band under the battery strip on detached themes.
  const fui::Rect line =
      batteryDetached ? fui::Rect{band.x, static_cast<int16_t>(band.y + metrics.batteryBarHeight), band.width,
                                  static_cast<int16_t>(bandH - metrics.batteryBarHeight - tokens.headerUnderline)}
                      : fui::Rect{band.x, band.y, band.width, static_cast<int16_t>(bandH - tokens.headerUnderline)};

  // Back button: label inside a bordered tab whose left end comes to a point
  // (iOS 6). The tab's body is a rounded rect open on the left; the arrow is
  // two strokes meeting at the tip, and the page text behind the tip is
  // already painted out with the band.
  const char* backLabel = tr(STR_LIBRARY);
  fui::TextStyle backStyle = tokens.smallText;
  backStyle.bold = true;
  backStyle.align = fui::TextAlign::Center;
  const int16_t textW = screen.target().measureText(backStyle.font, backLabel, backStyle).width;
  const int16_t btnH = kBackButtonH;
  const int16_t bodyW = static_cast<int16_t>(textW + 2 * kBackPadX);
  const int16_t btnX = static_cast<int16_t>(band.x + tokens.headerSidePadding);
  const int16_t btnY = static_cast<int16_t>(line.y + (line.height - btnH) / 2);
  const fui::Rect tab{static_cast<int16_t>(btnX + kBackArrow), btnY, bodyW, btnH};
  const uint8_t tabRadius = static_cast<uint8_t>(std::min<int>(tokens.controlRadius, 8));
  screen.target().fill(tab, paper, tabRadius, fui::CornerTopRight | fui::CornerBottomRight);
  screen.target().stroke(tab, ink, 2, tabRadius, fui::CornerTopRight | fui::CornerBottomRight);
  // Open the tab's left edge, then draw the arrow onto it: one 2px run per
  // row along the two diagonals (scanlines instead of drawn lines, so the
  // edge stays a clean 1-bit stair with no aliasing gaps).
  // (Only between the top and bottom borders, which the arrow's ends meet.)
  screen.target().fill(
      fui::Rect{static_cast<int16_t>(tab.x - 1), static_cast<int16_t>(tab.y + 2), 4, static_cast<int16_t>(btnH - 4)},
      paper);
  const int half = btnH / 2;
  for (int dy = 0; dy < btnH; ++dy) {
    const int dist = dy < half ? half - dy : dy - half;  // 0 at the tip row
    // Edge x for this row: the tip at dist 0, the tab body at dist == half.
    const int edgeX = btnX + (dist * kBackArrow + half / 2) / half;
    screen.target().fill(fui::Rect{static_cast<int16_t>(edgeX), static_cast<int16_t>(btnY + dy), 2, 1}, ink);
  }
  screen.target().text(tab, backLabel, backStyle);
  // Finger-sized target around the drawn button.
  const fui::Rect backHit{band.x, line.y, static_cast<int16_t>(kBackArrow + bodyW + 2 * tokens.headerSidePadding),
                          line.height};
  screen.frame().hit(backHit, ACTION_HOME, 0, fui::InputTouch);

  // Title, centred on the line between the button and the battery.
  if (model_.bookTitle) {
    fui::TextStyle titleStyle = tokens.titleText;
    titleStyle.bold = true;
    titleStyle.align = fui::TextAlign::Center;
    const int16_t leftEdge = static_cast<int16_t>(backHit.right() + tokens.spaceSm);
    const int16_t rightEdge =
        static_cast<int16_t>(batteryDetached ? band.right() - tokens.headerSidePadding : batteryX - tokens.spaceMd);
    // Keep the title centred on the band when there is room, else on the gap.
    const int16_t maxW = static_cast<int16_t>(rightEdge - leftEdge);
    const int16_t titleW =
        std::min<int16_t>(maxW, screen.target().measureText(titleStyle.font, model_.bookTitle, titleStyle).width);
    int16_t x = static_cast<int16_t>(band.x + (band.width - titleW) / 2);
    if (x < leftEdge) x = leftEdge;
    if (x + titleW > rightEdge) x = static_cast<int16_t>(rightEdge - titleW);
    const int16_t th = screen.target().lineHeight(titleStyle.font);
    screen.target().text(fui::Rect{x, static_cast<int16_t>(line.y + (line.height - th) / 2), titleW, th},
                         model_.bookTitle, titleStyle);
  }
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();

  buildHeader(screen);

  // Sheet height from its content: scrub row, meta line, tool row, and the
  // air between them.
  const int16_t metaH = screen.target().lineHeight(tokens.smallText.font);
  const int16_t contentH = static_cast<int16_t>(tokens.spaceMd + kScrubButton + tokens.spaceMd + metaH +
                                                tokens.spaceSm + kToolRowH + tokens.spaceSm);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  sheetProps.grabberMargin = tokens.spaceMd;
  sheetProps.grabberInset = tokens.spaceSm;
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  screen.sheet(sheetProps, static_cast<int16_t>(contentH + grabberBand));
  screen.insetContent(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});
  screen.spacer(tokens.spaceMd);

  // Scrub row: < [progress track + knob: tap/drag to jump] >
  {
    const fui::Rect band = screen.takeTop(kScrubButton, tokens.spaceMd);
    stepProps_.label = nullptr;
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_back_24);
    stepProps_.action = ACTION_PREV;
    stepProps_.inputMask = fui::InputTouch;
    stepProps_.styles.explicitlySet = true;
    stepProps_.styles.normal.background = fui::Paint::solid(fui::Color::White);
    stepProps_.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.borderWidth = 1;
    stepProps_.styles.normal.radius = static_cast<uint8_t>(std::min<int>(tokens.controlRadius, kScrubButton / 2));
    stepProps_.styles.selected = stepProps_.styles.normal;
    stepProps_.styles.focused = stepProps_.styles.normal;
    stepProps_.styles.disabled = stepProps_.styles.normal;
    stepProps_.styles.active = stepProps_.styles.normal;
    stepProps_.styles.active.background = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.active.foreground = fui::Paint::solid(fui::Color::White);
    screen.button(stepProps_, fui::Rect{band.x, band.y, kScrubButton, kScrubButton});
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_next_24);
    stepProps_.action = ACTION_NEXT;
    screen.button(stepProps_,
                  fui::Rect{static_cast<int16_t>(band.right() - kScrubButton), band.y, kScrubButton, kScrubButton});

    // Progress: a 2px track with a solid round knob at the book position (the
    // fill edge IS the handle, so nothing else is drawn), the scrub hit rect
    // spanning the whole band height so a finger a little off the line still
    // scrubs. Tap-to-jump / drag arrive as dragPermille along this rect.
    const int16_t trackX = static_cast<int16_t>(band.x + kScrubButton + kScrubGap);
    const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kScrubButton + kScrubGap));
    const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 1), trackW, 2}, ink);
    const int permille = std::clamp(model_.progressPermille, 0, 1000);
    const int16_t knobX = static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - 1) * permille) / 1000);
    screen.target().fill(fui::Rect{static_cast<int16_t>(knobX - kScrubKnob / 2),
                                   static_cast<int16_t>(trackY - kScrubKnob / 2), kScrubKnob, kScrubKnob},
                         ink, static_cast<uint8_t>(kScrubKnob / 2));
    screen.frame().hit(fui::Rect{trackX, band.y, trackW, band.height}, ACTION_SCRUB, 0,
                       fui::InputTouch | fui::InputDrag);
  }

  // Meta line: chapter title (left), chapter page / book percent (right).
  {
    const fui::Rect line = screen.takeTop(metaH, tokens.spaceSm);
    fui::TextStyle titleStyle = tokens.smallText;
    titleStyle.bold = true;
    fui::TextStyle infoStyle = tokens.smallText;
    infoStyle.align = fui::TextAlign::Right;
    const int16_t infoW =
        model_.pageInfo ? screen.target().measureText(infoStyle.font, model_.pageInfo, infoStyle).width : 0;
    const fui::Rect titleRect{line.x, line.y, static_cast<int16_t>(line.width - infoW - tokens.spaceMd), line.height};
    if (model_.chapterTitle) screen.target().text(titleRect, model_.chapterTitle, titleStyle);
    if (model_.pageInfo) screen.target().text(line, model_.pageInfo, infoStyle);
  }

  buildToolRow(screen, fui::LayoutAnchor::Top);
}

void ReaderToolbarUi::buildPanel(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();

  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;  // tap the page above the sheet = back to the toolbar
  sheetProps.grabberMargin = tokens.spaceMd;
  sheetProps.grabberInset = tokens.spaceMd;
  screen.sheet(sheetProps, static_cast<int16_t>((safe.height * kPanelHeightPercent) / 100));
  screen.insetContent(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});

  // Title line: panel name left, page position right when the list spans pages.
  {
    fui::TextStyle titleStyle = tokens.titleText;
    titleStyle.bold = true;
    const int16_t titleH = screen.target().lineHeight(titleStyle.font);
    const fui::Rect line = screen.takeTop(titleH, tokens.spaceMd);
    screen.target().text(line, model_.panelTitle, titleStyle);
    // Filled in below once the viewport is known; reserve the rect now.
    pageIndicatorRect_ = line;
  }

  // Switcher row along the sheet's bottom edge (above the button-hint row on
  // button boards); the list takes what is left.
  screen.spacer(static_cast<int16_t>(tokens.spaceSm + std::max(0, model_.bottomReserve)), fui::LayoutAnchor::Bottom);
  buildToolRow(screen, fui::LayoutAnchor::Bottom);
  screen.spacer(tokens.spaceSm, fui::LayoutAnchor::Bottom);

  listProps_.count = static_cast<uint16_t>(std::max(0, model_.itemCount));
  listProps_.action = ACTION_ROW;
  listProps_.inputMask = fui::InputTouch;  // physical buttons stay with the reader
  listProps_.rowHeight =
      model_.denseRows ? static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight) : tokens.rowHeight;
  const fui::Rect listRect = screen.body();
  const uint16_t rows = fui::listVisibleRows(listRect, listProps_.rowHeight, tokens.listRowGap);
  visibleRows_ = rows > 0 ? rows : 1;
  const int selected = std::clamp(model_.selectedIndex, -1, model_.itemCount - 1);
  if (model_.itemCount > 0) {
    // Follow the cursor (or the row the reader asked to show) into view.
    const int follow = selected >= 0 ? selected : std::min(topIndex_, model_.itemCount - 1);
    topIndex_ = fui::listTopIndexFor(static_cast<int16_t>(follow), static_cast<uint16_t>(std::max(0, topIndex_)),
                                     static_cast<uint16_t>(visibleRows_), static_cast<uint16_t>(model_.itemCount));
  } else {
    topIndex_ = 0;
  }

  // Materialise only the visible window of rows.
  const int windowCount = std::min({visibleRows_, model_.itemCount - topIndex_, kMaxWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = topIndex_ + i;
    windowLabels_[i] = model_.rowText ? model_.rowText(index) : std::string();
    windowValues_[i] = model_.rowValue ? model_.rowValue(index) : std::string();
    fui::ListItem item;
    item.label = windowLabels_[i].c_str();
    item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
    item.actionValue = static_cast<int16_t>(index);
    windowItems_[i] = item;
  }
  listProps_.items = windowItems_;
  listProps_.itemsWindowFirst = static_cast<uint16_t>(topIndex_);
  listProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  listProps_.topIndex = static_cast<uint16_t>(topIndex_);
  listProps_.selectedIndex = static_cast<int16_t>(selected);
  listProps_.valueText = tokens.bodyText;
  listProps_.valueText.bold = true;
  if (model_.itemCount > 0) {
    screen.list(listProps_);
  }

  const int totalPages = visibleRows_ > 0 ? (model_.itemCount + visibleRows_ - 1) / visibleRows_ : 0;
  if (totalPages > 1) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", topIndex_ / visibleRows_ + 1, totalPages);
    fui::TextStyle pageStyle = tokens.smallText;
    pageStyle.align = fui::TextAlign::Right;
    screen.target().text(pageIndicatorRect_, buf, pageStyle);
  }
}
