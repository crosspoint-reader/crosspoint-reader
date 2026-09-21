#include "CoverGridHomeUi.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/blocks.h"
#include "icons/book.h"
#include "icons/folder.h"
#include "icons/library.h"
#include "icons/settings2.h"
#include "icons/transfer.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId SELECT = 1;
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
}  // namespace

CoverGridHomeUi::CoverGridHomeUi(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void CoverGridHomeUi::begin(const std::vector<RecentBook>& recent, bool opds, bool continuing) {
  books = &recent;
  hasOpds = opds;
  hasContinueReading = continuing;
  if (!recent.empty()) {
    // Cover regions do not overlap. Each may widen by one physical byte per row.
    coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                         coverPaths.size() * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
    coverCache = HalMemory::allocatePsram(coverCacheCapacity);
    if (!coverCache) {
      LOG_ERR("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
      coverCacheCapacity = 0;
    } else {
      LOG_DBG("HOME", "Cover cache: %u bytes in PSRAM", unsigned(coverCacheCapacity));
    }
  }
  resetUi();
  app.on(SELECT, &CoverGridHomeUi::onAction, this);
  app.setScreen(&CoverGridHomeUi::screenFn, this);
  if (lastThumbSpecOrientation == static_cast<int>(renderer.getOrientation())) thumbHeights = lastThumbHeights;
  refreshCoverPaths();
  progress = hasContinueReading ? loadProgress() : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CoverGridHomeUi::invalidateCoverCache() {
  coverCacheUsed = 0;
  for (auto& cached : cachedCovers) cached = CachedCover{};
}

void CoverGridHomeUi::refreshCoverPaths() {
  invalidateCoverCache();
  for (size_t i = 0; i < books->size() && i < coverPaths.size(); ++i) refreshCoverPath(i);
}

void CoverGridHomeUi::refreshCoverPath(size_t index) {
  if (index >= books->size() || index >= coverPaths.size()) return;
  cachedCovers[index].valid = false;
  coverPaths[index] = thumbHeights[index] > 0
                          ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, thumbHeights[index])
                          : std::string();
  if (index != 0) return;
  featuredCoverWidth = featuredCoverHeight = 0;
  if (!coverPaths[0].empty() && Storage.exists(coverPaths[0].c_str()) &&
      Storage.openFileForRead("HOME", coverPaths[0], coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok) {
      featuredCoverWidth = coverBitmap.getWidth();
      featuredCoverHeight = coverBitmap.getHeight();
    }
    coverFile.close();
  }
}

int CoverGridHomeUi::thumbHeightFor(size_t index) const {
  return index < thumbHeights.size() && thumbHeights[index] > 0 ? thumbHeights[index] : THUMB_HEIGHT;
}

bool CoverGridHomeUi::takeThumbHeightsChanged() {
  const bool changed = thumbHeightsChanged;
  thumbHeightsChanged = false;
  return changed;
}

void CoverGridHomeUi::noteThumbHeight(size_t index, int slotWidth, int slotHeight) {
  if (index >= thumbHeights.size()) return;
  // Thumbs cover a (0.6*h, h) target box, so a height of max(h, w*5/3) makes
  // every cover overfill the slot; paintCover crops the overflow (full bleed).
  const int height = std::max({1, slotHeight, slotWidth * 5 / 3 + 2});
  if (thumbHeights[index] != height) {
    thumbHeights[index] = height;
    thumbHeightsChanged = true;
    refreshCoverPath(index);
  }
  lastThumbHeights[index] = height;
  lastThumbSpecOrientation = static_cast<int>(renderer.getOrientation());
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
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverCacheOrientation != orientation) {
    invalidateCoverCache();
    coverCacheOrientation = orientation;
  }
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.insetContent(fui::Insets{theme.spaceSm, theme.spaceLg, theme.spaceSm, theme.spaceLg});
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const auto header = screen.takeTop(UITheme::getInstance().getMetrics().batteryBarHeight);
  auto tabRect = screen.takeBottom(56, theme.spaceSm);
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
  drawGrid(screen, screen.body());
  const auto gridRect = layoutGrid(screen, screen.body());
  tabRect.x = gridRect.x + grid.cellInset.left;
  tabRect.width = gridRect.width - grid.cellInset.left - grid.cellInset.right;
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
  card.state = selected == 0 ? fui::StateSelected : fui::StateNormal;
  card.selectionIndicator = fui::BookCardSelectionIndicator::CoverFrame;
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = theme.spaceLg + theme.spaceSm;
  card.coverSize.height = std::max(1, std::min(rect.height - 12, (rect.width / 3) * 5 / 3));
  card.coverSize.width = std::max(1, card.coverSize.height * 3 / 5);
  noteThumbHeight(0, card.coverSize.width, card.coverSize.height);
  // Generation bounds stay stable; the selection frame follows the actual image.
  if (featuredCoverWidth > 0 && featuredCoverHeight > 0) {
    const float scale = std::min(1.0f, std::min(float(card.coverSize.width) / featuredCoverWidth,
                                                float(card.coverSize.height) / featuredCoverHeight));
    card.coverSize.width = std::max(1, static_cast<int>(featuredCoverWidth * scale));
    card.coverSize.height = std::max(1, static_cast<int>(featuredCoverHeight * scale));
  }
  const auto gridRect = layoutGrid(screen, screen.body());
  rect.x = gridRect.x;
  rect.width = gridRect.width;
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, 0);
  };
  fui::bookCard(screen.frame(), rect, card);
}

fui::Rect CoverGridHomeUi::layoutGrid(UiScreen& screen, fui::Rect rect) {
  const auto& theme = screen.theme();
  grid.gap = std::max<int>(theme.spaceSm, rect.width * 2 / 100);
  grid.rowGap = theme.spaceSm;
  grid.cellInset = fui::Insets{6, 6, 6, 6};
  const int maxCoverWidth = std::max(1, (rect.width - (GRID_COLUMNS - 1) * grid.gap) / GRID_COLUMNS - 12);
  const int maxCoverHeight = std::max(1, (rect.height - (GRID_ROWS - 1) * grid.rowGap) / GRID_ROWS - 12);
  grid.coverSize.height = std::max(1, std::min({maxCoverHeight, maxCoverWidth * 5 / 3, card.coverSize.height * 3 / 2}));
  grid.coverSize.width = std::max(1, grid.coverSize.height * 3 / 5);
  grid.rowHeight = grid.coverSize.height + 12;
  rect.height = GRID_ROWS * grid.rowHeight + (GRID_ROWS - 1) * grid.rowGap;
  return rect;
}

void CoverGridHomeUi::drawGrid(UiScreen& screen, fui::Rect rect) {
  grid.count = books->size() > 1 ? books->size() - 1 : 0;
  grid.columns = GRID_COLUMNS;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = selected > 0 && selected < static_cast<int>(books->size()) ? selected - 1 : -1;
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.labelHeight = 0;
  grid.labelGap = 0;
  rect = layoutGrid(screen, rect);
  for (size_t i = 1; i < thumbHeights.size(); ++i) noteThumbHeight(i, grid.coverSize.width, grid.coverSize.height);
  grid.scrollIndicator = false;
  grid.itemProvider = [](uint16_t index, void*) { return fui::coverGridItem(nullptr, index + 1); };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::CoverGridItem&, uint16_t index,
                         void* user) {
    return static_cast<CoverGridHomeUi*>(user)->paintFramedCover(target, cover, index + 1);
  };
  // Distribute the unused width between columns, keeping both outside edges fixed.
  const int16_t cellWidth = grid.coverSize.width + grid.cellInset.left + grid.cellInset.right;
  const int16_t travel = std::max<int>(0, rect.width - cellWidth);
  grid.columns = 1;
  for (uint16_t index = 0; index < grid.count; ++index) {
    const int column = index % GRID_COLUMNS;
    const int row = index / GRID_COLUMNS;
    grid.topIndex = index;
    const fui::Rect cell{static_cast<int16_t>(rect.x + column * travel / (GRID_COLUMNS - 1)),
                         static_cast<int16_t>(rect.y + row * (grid.rowHeight + grid.rowGap)), cellWidth,
                         grid.rowHeight};
    fui::coverGrid(screen.frame(), cell, grid);
  }
  grid.topIndex = 0;
  grid.columns = GRID_COLUMNS;
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
  tabs.count = 1;
  tabs.action = SELECT;
  tabs.inputMask = fui::InputTouch;
  tabs.iconSize = 32;
  tabs.iconPainterUserData = this;
  tabs.iconPainter = [](fui::DrawTarget&, fui::Rect iconRect, const fui::TabItem& tab, uint8_t, void* user) {
    auto& self = *static_cast<CoverGridHomeUi*>(user);
    const int index = tab.value - static_cast<int>(self.books->size());
    const int icon = !self.hasOpds && index >= 2 ? index + 1 : index;
    self.renderer.drawIcon(ICONS[icon], iconRect.x, iconRect.y, iconRect.width);
    return true;
  };
  tabs.tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabs.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
  tabs.selectedUnderline = 2;
  const int16_t slotWidth = std::max<int16_t>(tabs.minTouchSize, tabs.iconSize);
  const int16_t slotInset = (slotWidth - tabs.iconSize) / 2;
  const int16_t travel = std::max<int16_t>(0, rect.width - tabs.iconSize);
  for (int i = 0; i < count; ++i) {
    tabs.tabs = &tabItems[i];
    const int16_t x = rect.x - slotInset + i * travel / (count - 1);
    fui::tabBar(screen.frame(), fui::Rect{x, rect.y, slotWidth, rect.height}, tabs);
  }
}

bool CoverGridHomeUi::paintFramedCover(fui::DrawTarget& target, fui::Rect rect, size_t index) {
  constexpr int16_t SHADOW_OFFSET = 2;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + SHADOW_OFFSET), SHADOW_OFFSET, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + SHADOW_OFFSET), rect.bottom(), rect.width, SHADOW_OFFSET}, ink);
  const bool drawn = paintCover(rect, index);
  target.stroke(rect, ink, 1, 0);
  return drawn;
}

bool CoverGridHomeUi::paintCover(fui::Rect rect, size_t index) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, coverCache.get() + cached.offset,
                                  cached.bytes)) {
    return true;
  }
  cached.valid = false;
  bool drawn = false;
  if (index < coverPaths.size() && !coverPaths[index].empty() &&
      Storage.openFileForRead("HOME", coverPaths[index], coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok && coverBitmap.getWidth() > 0 && coverBitmap.getHeight() > 0) {
      drawn = GUI.drawCoverThumbFill(renderer, coverBitmap, Rect{rect.x, rect.y, rect.width, rect.height});
    }
    coverFile.close();
  }
  if (!drawn) GUI.drawCoverPlaceholder(renderer, Rect{rect.x, rect.y, rect.width, rect.height});
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid = renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height,
                                                 coverCache.get() + cached.offset, cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}

int CoverGridHomeUi::loadProgress() const {
  if (books->empty()) return -1;
  const auto& path = books->front().path;
  uint8_t data[10]{};
  if (FsHelpers::hasEpubExtension(path)) {
    // Metadata objects exceed the stack budget; only the featured book is loaded, once per entry.
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: progress metadata");
      return -1;
    }
    if (!epub->load(false, true)) return -1;
    HalFile file;
    if (!Storage.openFileForRead("HOME", epub->getCachePath() + "/progress.bin", file)) return -1;
    const int size = file.read(data, sizeof(data));
    if (size != 4 && size != 6 && size != 10) return -1;
    const int spine = data[0] | (data[1] << 8);
    const int page = data[2] | (data[3] << 8);
    const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
    if (epub->getSpineItemsCount() <= 0 || epub->getBookSize() == 0) return -1;
    if (spine == epub->getSpineItemsCount()) return 100;
    if (spine > epub->getSpineItemsCount()) return -1;
    const float fraction = total > 0 && page != UINT16_MAX ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
    return std::clamp(static_cast<int>(epub->calculateProgress(spine, fraction) * 100 + 0.5f), 0, 100);
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: XTC progress metadata");
      return -1;
    }
    if (!xtc->load()) return -1;
    HalFile file;
    if (!Storage.openFileForRead("HOME", xtc->getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    const uint32_t page = readLe32(data);
    if (xtc->getPageCount() == 0) return -1;
    if (page >= xtc->getPageCount()) return 100;
    return xtc->calculateProgress(page);
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    Txt txt(path, "/.crosspoint");
    HalFile file;
    if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
      return -1;
    const uint32_t page = data[0] | (data[1] << 8);
    HalFile index;
    // TXT index v3: magic, version, file size, four layout fields, alignment, page count.
    uint8_t header[30];
    if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/index.bin", index) ||
        index.read(header, sizeof(header)) != sizeof(header))
      return -1;
    if (readLe32(header) != 0x54585449 || header[4] != 3) return -1;
    const uint32_t pages = readLe32(header + 26);
    if (pages == 0 || pages > (index.size() - sizeof(header)) / 4) return -1;
    HalFile source;
    if (!Storage.openFileForRead("HOME", path, source) || source.size() != readLe32(header + 5)) return -1;
    return std::min<int>(100, static_cast<int>((page + 1) * 100ULL / pages));
  }
  return -1;
}
