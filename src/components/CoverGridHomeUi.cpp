#include "CoverGridHomeUi.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/blocks.h"
#include "icons/book.h"
#include "icons/folder.h"
#include "icons/library.h"
#include "icons/settings2.h"
#include "icons/transfer.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
  refreshCoverPaths();
  progress = hasContinueReading && !books->empty() ? loadBookProgress(books->front().path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::refreshCoverPaths() {
  coverCache.invalidate();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  coverCache.invalidate(index);
  coverPaths[index] = thumbHeights[index] > 0
                          ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeights[index])
                          : std::string();
  if (index != 0) return;
  coverCache.readSize(coverPaths[0], featuredCoverWidth, featuredCoverHeight);
}

int CoverGridHomeUi::thumbHeightFor(size_t index) const {
  return index < thumbHeights.size() && thumbHeights[index] > 0 ? thumbHeights[index] : THUMB_HEIGHT;
}

bool CoverGridHomeUi::takeThumbHeightsChanged() { return std::exchange(thumbHeightsChanged, false); }

void CoverGridHomeUi::noteThumbHeight(size_t index, int slotWidth, int slotHeight) {
  if (index >= thumbHeights.size()) return;
  // Thumbs cover a (0.6*h, h) target box, so a height of max(h, w*5/3) makes
  // every cover overfill the slot; the cover renderer crops the overflow (full bleed).
  const int height = std::max({1, slotHeight, slotWidth * 5 / 3 + 2});
  if (thumbHeights[index] != height) {
    thumbHeights[index] = height;
    thumbHeightsChanged = true;
    refreshCoverPath(index);
  }
}

void CoverGridHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CoverGridHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int CoverGridHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void CoverGridHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CoverGridHomeUi*>(user)->draw(screen); }

void CoverGridHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const auto header = screen.takeTop(UITheme::getInstance().getMetrics().batteryBarHeight);
  auto tabRect = screen.takeBottom(UITheme::getInstance().getMetrics().coverGridTabBarHeight, theme.spaceMd);
  if (books->empty()) {
    drawTabs(screen, tabRect.inset(fui::Insets{0, 6, 0, 6}));
    drawEmpty(screen);
    drawHeaderBand(header, tabRect.x + 6, tabRect.x + tabRect.width - 6);
    return;
  }
  auto headingText = theme.titleText;
  headingText.bold = true;
  auto headingRect = screen.takeTop(screen.target().lineHeight(headingText.font), theme.spaceSm);
  // Bound the featured section while leaving room for its metadata.
  const int16_t featuredHeight = std::min<int>(
      screen.body().height, std::max<int>(std::min<int>(240, screen.body().height * 3 / 10),
                                          screen.target().lineHeight(theme.bodyText.font) * (landscape ? 1 : 2) +
                                              screen.target().lineHeight(theme.smallText.font) * 2 + 32));
  drawCurrent(screen, screen.takeTop(featuredHeight, theme.spaceMd));
  drawGrid(screen);
  const auto& gridRect = gridBounds;
  tabRect.x = gridRect.x + grid.cellInset.left;
  tabRect.width = gridRect.width - grid.cellInset.left - grid.cellInset.right;
  // Heading shares the clock's alignment line (drawHeaderBand lands the
  // status content on the outer cover columns, i.e. this same x). The
  // selection ring extends left of this line by design — it reads as a frame
  // around the cover, not as the column edge.
  headingRect.x = tabRect.x;
  headingRect.width = tabRect.width;
  screen.target().text(headingRect, hasContinueReading ? tr(STR_CONTINUE_READING) : tr(STR_START_READING), headingText);
  drawTabs(screen, tabRect);
  // Drawn last so it can borrow the grid geometry, like the tab bar above.
  drawHeaderBand(header, tabRect.x, tabRect.x + tabRect.width);
}

void CoverGridHomeUi::drawHeaderBand(fui::Rect header, int coverLeft, int coverRight) {
  // Same alignment trick as the tabs: the clock's left edge and the battery's
  // right edge sit on the outer cover columns. drawHeader anchors both at
  // headerStatusInset() from the band edges, and the clock text is
  // left-anchored, so 1- vs 2-digit hours never move it.
  const int inset = GUI.headerStatusInset();
  const int headerX = std::max(0, coverLeft - inset);
  const int headerRight = std::min<int>(renderer.getScreenWidth(), coverRight + inset);
  GUI.drawHeader(renderer, Rect{headerX, header.y, headerRight - headerX, header.height}, nullptr);
}

void CoverGridHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.body();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  constexpr int16_t ICON_SIZE = 32;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  int16_t y = body.y + std::max(0, (body.height - contentHeight) / 2);
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y += ICON_SIZE + theme.spaceLg;
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y += titleHeight + theme.spaceSm;
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_START_READING), message);
}

void CoverGridHomeUi::drawCurrent(UiScreen& screen, fui::Rect rect) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = book.author.empty() ? nullptr : book.author.c_str();
  card.meta = nullptr;
  card.progressLabel = progress >= 0 ? progressText : nullptr;
  card.centerTextOnCover = true;
  card.progress = std::max(0, progress);
  card.progressMax = progress >= 0 ? 100 : 0;
  card.action = SELECT;
  // The featured card's selected state is a slim accent bar drawn after the
  // card (see below), not a bookCard indicator: every ring/outline treatment
  // tried here either overwhelmed the large cover or made the heading above
  // read as misaligned.
  card.state = fui::StateNormal;
  card.styles = theme.listRow;
  card.styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
  // The grid thumbs' selection ring draws with this border: gray like Lyra's
  // selection box, not solid black.
  card.styles.selected.border = fui::Paint::dither(fui::Color::LightGray);
  card.styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  card.styles.selected.radius = theme.listRowRadius;
  card.styles.active = card.styles.selected;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  card.coverSize.height = std::max(1, std::min(rect.height - 12, (rect.width / 3) * 5 / 3));
  card.coverSize.width = std::max(1, card.coverSize.height * 3 / 5);
  noteThumbHeight(0, card.coverSize.width, card.coverSize.height);
  // Generation bounds stay stable; the displayed cover follows the actual image.
  if (featuredCoverWidth > 0 && featuredCoverHeight > 0) {
    const float scale = std::min(1.0f, std::min(float(card.coverSize.width) / featuredCoverWidth,
                                                float(card.coverSize.height) / featuredCoverHeight));
    card.coverSize.width = std::max(1, static_cast<int>(featuredCoverWidth * scale));
    card.coverSize.height = std::max(1, static_cast<int>(featuredCoverHeight * scale));
  }
  gridBounds = layoutGrid(screen, screen.body());
  rect.x = gridBounds.x;
  rect.width = gridBounds.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
  fui::bookCard(screen.frame(), rect, card);

  if (selected == 0 && !BoardConfig::hasTouch()) {
    // Button boards only: a vertical accent bar left of the card, cover-height
    // and vertically centered on it, marks the featured card as the button
    // cursor without framing the cover. Touch boards tap directly and need no
    // cursor on the hero card.
    const int16_t barH = card.coverSize.height;
    screen.target().fill(
        fui::Rect{static_cast<int16_t>(rect.x - 9), static_cast<int16_t>(rect.y + (rect.height - barH) / 2), 3, barH},
        fui::Paint::dither(fui::Color::LightGray));
  }
}

fui::Rect CoverGridHomeUi::layoutGrid(UiScreen& screen, fui::Rect rect) {
  const auto& theme = screen.theme();
  grid.gap = std::max<int>(theme.spaceSm, rect.width * 2 / 100);
  grid.rowGap = grid.gap;
  grid.cellInset = fui::Insets{6, 6, 6, 6};
  const int maxCoverWidth = std::max(1, (rect.width - (GRID_COLUMNS - 1) * grid.gap) / GRID_COLUMNS - 12);
  const int maxCoverHeight = std::max(1, (rect.height - (GRID_ROWS - 1) * grid.rowGap) / GRID_ROWS - 12);
  grid.coverSize.height = std::max(1, std::min({maxCoverHeight, maxCoverWidth * 5 / 3, card.coverSize.height * 3 / 2}));
  grid.coverSize.width = std::max(1, grid.coverSize.height * 3 / 5);
  grid.rowHeight = grid.coverSize.height + 12;
  const int gridWidth = GRID_COLUMNS * (grid.coverSize.width + 12) + (GRID_COLUMNS - 1) * grid.gap;
  rect.x += (rect.width - gridWidth) / 2;
  rect.width = gridWidth;
  rect.height = GRID_ROWS * grid.rowHeight + (GRID_ROWS - 1) * grid.rowGap;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen) {
  const auto rect = gridBounds;
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
  grid.columns = GRID_COLUMNS;
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  // Same thick cover ring as the featured card; the dithered Cell background
  // was easy to miss behind a dark cover.
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  // Thick dithered ring sized for the small thumbs: 6px outside the cover,
  // 2px over its edge.
  grid.selectedCoverFrameGap = 6;
  grid.selectedCoverFrameWidth = 8;
  grid.cellStyles = card.styles;
  grid.labelHeight = 0;
  grid.labelGap = 0;
  for (size_t i = 1; i < thumbHeights.size(); ++i) noteThumbHeight(i, grid.coverSize.width, grid.coverSize.height);
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  fui::coverGrid(screen.frame(), rect, grid);
}

void CoverGridHomeUi::drawTabs(UiScreen& screen, fui::Rect rect) {
  static constexpr const uint8_t* ICONS[] = {FolderIcon, LibraryIcon, BlocksIcon, TransferIcon, Settings2Icon};
  int count = 0;
  for (int i = 0; i < 5; ++i) {
    if (i == 2 && !hasOpds) continue;
    auto& tab = tabItems[count];
    tab.value = books->size() + count;
    tab.selected = selected == tab.value;
    tab.label = nullptr;
    ++count;
  }
  tabs.tabs = tabItems.data();
  tabs.count = count;
  tabs.layout = fui::TabBarLayout::SpaceBetween;
  tabs.action = SELECT;
  tabs.inputMask = fui::InputTouch;
  tabs.iconSize = 32;
  tabs.iconPainterUserData = this;
  tabs.iconPainter = [](fui::DrawTarget&, fui::Rect iconRect, const fui::TabItem& tab, uint8_t, void* user) {
    const auto& self = *static_cast<CoverGridHomeUi*>(user);
    const int index = tab.value - static_cast<int>(self.books->size());
    const int icon = !self.hasOpds && index >= 2 ? index + 1 : index;
    self.renderer.drawIcon(ICONS[icon], iconRect.x, iconRect.y, iconRect.width);
    return true;
  };
  tabs.tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabs.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
  tabs.selectedUnderline = 2;
  tabs.distributedSlotWidth = 0;
  fui::tabBar(screen.frame(), rect, tabs);
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget& target, fui::Rect rect, size_t index) {
  constexpr int16_t SHADOW_OFFSET = 2;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + SHADOW_OFFSET), SHADOW_OFFSET, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + SHADOW_OFFSET), rect.bottom(), rect.width, SHADOW_OFFSET}, ink);
  const bool drawn = index < coverPaths.size() && coverCache.paint(rect, index, coverPaths[index]);
  target.stroke(rect, ink, 1, 0);
  return drawn;
}
