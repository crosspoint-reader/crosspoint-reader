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
constexpr fui::ActionId ACTION_SCRUB = 5;    // capsule: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;      // panel list row, value = row index

constexpr int16_t kScrubControlHeight = 56;  // the capsule + its step buttons
constexpr int16_t kToolTileHeight = 56;
constexpr int16_t kToolTileGap = 12;
constexpr int kToolCount = 3;
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
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_ROW; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() { renderUi(); }

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub capsule is a drag target, so held frames must reach it.
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
  return fui::tileGridHeight(kToolCount, kToolCount, kToolTileHeight, kToolTileGap);
}

// The Contents / Text / More row: three tiles, the active one filled. Tile
// values are the tool ids, so the reader's focusedTool maps straight onto them.
void ReaderToolbarUi::buildToolRow(UiScreen& screen, const fui::LayoutAnchor anchor) {
  const char* labels[kToolCount] = {tr(STR_TOOL_CONTENTS), tr(STR_TOOL_TEXT), tr(STR_TOOL_MORE)};
  const fui::BitmapRef icons[kToolCount] = {fui::bitmapFromIcon(icon_reader_contents_24),
                                            fui::bitmapFromIcon(icon_reader_text_24),
                                            fui::bitmapFromIcon(icon_reader_more_24)};
  for (int i = 0; i < kToolCount; ++i) {
    toolItems_[i].label = labels[i];
    toolItems_[i].icon = icons[i];
    toolItems_[i].value = static_cast<int16_t>(i);
    toolItems_[i].state = i == model_.activeTool ? fui::StateChecked : fui::StateNormal;
  }
  toolProps_.items = toolItems_;
  toolProps_.count = kToolCount;
  toolProps_.columns = kToolCount;
  toolProps_.action = ACTION_TOOL;
  toolProps_.tileHeight = kToolTileHeight;
  toolProps_.gap = kToolTileGap;
  toolProps_.iconSize = 24;
  toolProps_.text = screen.theme().smallText;
  toolProps_.text.bold = true;
  screen.tileGrid(toolProps_, anchor);
}

// Top bar: white band with the book title centred and the battery on the
// right. No button of its own — the sheet registers everything outside
// itself (this bar included) as the dismiss target, which is what a tap up
// here means anyway.
void ReaderToolbarUi::buildHeader(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect body = screen.body();
  const fui::Rect band{body.x, body.y, body.width, tokens.headerHeight};

  const uint16_t percent = powerManager.getBatteryPercentage();
  const bool showPercent = SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  batteryText_ = std::to_string(percent) + "%";
  const int16_t labelW =
      showPercent ? screen.target().measureText(tokens.smallText.font, batteryText_.c_str(), tokens.smallText).width
                  : 0;
  constexpr int16_t kBatteryGap = 4;  // BaseTheme::batteryPercentSpacing
  // The icon glyph extends 2px past glyphWidth (terminal nub); reserve it or
  // the percent label's rect comes up short and the text truncates.
  constexpr int16_t kBatteryNubWidth = 2;
  const int16_t batteryReserve =
      static_cast<int16_t>(metrics.batteryWidth + kBatteryNubWidth + (showPercent ? labelW + kBatteryGap : 0));

  // The page text runs under this band: paint it out before the header draws.
  screen.target().fill(band, fui::Paint::solid(fui::Color::White));
  headerProps_.title = model_.bookTitle;
  headerProps_.centered = true;
  headerProps_.borderEdges = fui::EdgeBottom;
  headerProps_.rightReserve = static_cast<int16_t>(batteryReserve + tokens.spaceMd);
  headerProps_.leftReserve = headerProps_.rightReserve;  // keep the title centred on the band
  screen.header(headerProps_);

  fui::BatteryIndicatorProps battery;
  battery.percent = static_cast<uint8_t>(percent > 100 ? 100 : percent);
  battery.charging = gpio.isUsbConnected();
  battery.label = showPercent ? batteryText_.c_str() : nullptr;
  battery.text = tokens.smallText;
  battery.glyphWidth = static_cast<int16_t>(metrics.batteryWidth);
  battery.glyphHeight = static_cast<int16_t>(metrics.batteryHeight);
  battery.gap = kBatteryGap;
  const int16_t batteryX = static_cast<int16_t>(band.right() - tokens.headerSidePadding - batteryReserve);
  fui::batteryIndicator(screen.frame(), fui::Rect{batteryX, band.y, batteryReserve, tokens.headerHeight}, battery);
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();

  buildHeader(screen);

  // Scrub row: chapter title as the caption, page/percent as the readout,
  // previous/next chapter on the step buttons, the book's progress on the
  // capsule (tap or drag to jump).
  scrubProps_.label = model_.chapterTitle;
  scrubProps_.value = model_.pageInfo;
  scrubProps_.sliderValue = std::clamp(model_.progressPermille, 0, 1000);
  scrubProps_.max = 1000;
  scrubProps_.sliderAction = ACTION_SCRUB;
  scrubProps_.decrement = ACTION_PREV;
  scrubProps_.increment = ACTION_NEXT;
  scrubProps_.decrementLabel = "<";
  scrubProps_.incrementLabel = ">";
  scrubProps_.labelText = tokens.smallText;
  scrubProps_.labelText.bold = true;
  scrubProps_.valueText = tokens.smallText;

  // Sheet height from its content: air, scrub row, air, tool row, air.
  const int16_t scrubH = fui::sliderRowHeight(screen.target(), scrubProps_, kScrubControlHeight);
  const int16_t contentH =
      static_cast<int16_t>(tokens.spaceLg + scrubH + tokens.spaceMd + toolRowHeight(screen) + tokens.spaceMd);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  sheetProps.grabberMargin = tokens.spaceMd;
  sheetProps.grabberInset = tokens.spaceMd;
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  screen.sheet(sheetProps, static_cast<int16_t>(contentH + grabberBand));
  screen.insetContent(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});

  screen.spacer(tokens.spaceLg);
  screen.sliderRow(scrubProps_, kScrubControlHeight);
  screen.spacer(tokens.spaceMd);
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
  screen.spacer(static_cast<int16_t>(tokens.spaceMd + std::max(0, model_.bottomReserve)), fui::LayoutAnchor::Bottom);
  buildToolRow(screen, fui::LayoutAnchor::Bottom);
  screen.spacer(tokens.spaceMd, fui::LayoutAnchor::Bottom);

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
