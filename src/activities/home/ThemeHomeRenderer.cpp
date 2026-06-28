#include "ThemeHomeRenderer.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBookCoverPainter.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCacheBleed = 12;

const char* defaultLauncherLabel(ThemeHomeAction action) {
  switch (action) {
    case ThemeHomeAction::RecentBooks:
      return tr(STR_MENU_RECENT_BOOKS);
    case ThemeHomeAction::OpdsBrowser:
      return tr(STR_OPDS_BROWSER);
    case ThemeHomeAction::FileTransfer:
      return tr(STR_FILE_TRANSFER);
    case ThemeHomeAction::Settings:
      return tr(STR_SETTINGS_TITLE);
    case ThemeHomeAction::RecentBook:
      return tr(STR_CONTINUE_READING);
    case ThemeHomeAction::FileBrowser:
    default:
      return tr(STR_BROWSE_FILES);
  }
}

const char* buttonHintLabel(ThemeButtonHintLabel label, const char* fallback) {
  switch (label) {
    case ThemeButtonHintLabel::Empty:
      return "";
    case ThemeButtonHintLabel::Back:
      return tr(STR_BACK);
    case ThemeButtonHintLabel::Home:
      return tr(STR_HOME);
    case ThemeButtonHintLabel::Select:
      return tr(STR_SELECT);
    case ThemeButtonHintLabel::Confirm:
      return tr(STR_CONFIRM);
    case ThemeButtonHintLabel::Open:
      return tr(STR_OPEN);
    case ThemeButtonHintLabel::Toggle:
      return tr(STR_TOGGLE);
    case ThemeButtonHintLabel::Up:
      return tr(STR_DIR_UP);
    case ThemeButtonHintLabel::Down:
      return tr(STR_DIR_DOWN);
    case ThemeButtonHintLabel::Left:
      return tr(STR_DIR_LEFT);
    case ThemeButtonHintLabel::Right:
      return tr(STR_DIR_RIGHT);
    case ThemeButtonHintLabel::Default:
    default:
      return fallback;
  }
}

UIIcon defaultLauncherIcon(ThemeHomeAction action) {
  switch (action) {
    case ThemeHomeAction::RecentBooks:
      return UIIcon::Recent;
    case ThemeHomeAction::OpdsBrowser:
      return UIIcon::Library;
    case ThemeHomeAction::FileTransfer:
      return UIIcon::Transfer;
    case ThemeHomeAction::Settings:
      return UIIcon::Settings;
    case ThemeHomeAction::RecentBook:
      return UIIcon::Book;
    case ThemeHomeAction::FileBrowser:
    default:
      return UIIcon::Folder;
  }
}

std::string homeHeaderTitle(const ThemeMetrics& metrics, const std::vector<RecentBook>& recentBooks,
                            const int coverSelectorIndex) {
  if (metrics.homeContinueReadingInMenu && metrics.homeShowContinueReadingHeader && !recentBooks.empty()) {
    return recentBooks[std::min(coverSelectorIndex, static_cast<int>(recentBooks.size()) - 1)].title;
  }
  return "";
}

Rect placedWidgetRect(Rect slot, const ThemeHomeWidgetSpec& widget) {
  slot.x += widget.offsetX - widget.bleed.left;
  slot.y += widget.offsetY - widget.bleed.top;
  slot.width += widget.bleed.left + widget.bleed.right;
  slot.height += widget.bleed.top + widget.bleed.bottom;
  slot.x += widget.inset.left;
  slot.y += widget.inset.top;
  slot.width -= widget.inset.left + widget.inset.right;
  slot.height -= widget.inset.top + widget.inset.bottom;
  return slot;
}

freeink::ui::Insets toFreeInkInsets(const ThemeEdgeInsets& insets) {
  return freeink::ui::makeInsets(insets.top, insets.right, insets.bottom, insets.left);
}

freeink::ui::StyleSet widgetSelectionStyles(ThemeWidgetSelectionStyle selectionStyle, int selectedRadius) {
  if (selectionStyle == ThemeWidgetSelectionStyle::Outline) {
    return freeink::ui::selectedOutlineListRowStyles(selectedRadius);
  }
  if (selectionStyle == ThemeWidgetSelectionStyle::None) return freeink::ui::selectedPlainListRowStyles();
  return freeink::ui::defaultListRowStyles();
}

freeink::ui::CoverGridSelectionIndicator coverGridSelectionIndicator(ThemeWidgetSelectionStyle selectionStyle) {
  return selectionStyle == ThemeWidgetSelectionStyle::CoverFrame ? freeink::ui::CoverGridSelectionIndicator::CoverFrame
                                                                 : freeink::ui::CoverGridSelectionIndicator::Cell;
}

const uint8_t* homeTabIcon(UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    default:
      return nullptr;
  }
}

struct HomeIconTabPainterData {
  const GfxRenderer* renderer = nullptr;
  const ThemeHomeLauncherSpec* const* launchers = nullptr;
  size_t launcherCount = 0;
};

struct HomeCoverGridItemProviderData {
  const std::vector<RecentBook>* recentBooks = nullptr;
  int startIndex = 0;
};

freeink::ui::CoverGridItem provideHomeCoverGridItem(uint16_t index, void* userData) {
  auto* data = static_cast<HomeCoverGridItemProviderData*>(userData);
  if (data == nullptr || data->recentBooks == nullptr) return {};
  const int bookIndex = data->startIndex + static_cast<int>(index);
  if (bookIndex < 0 || bookIndex >= static_cast<int>(data->recentBooks->size())) return {};
  return freeink::ui::coverGridItem((*data->recentBooks)[bookIndex].title.c_str(), bookIndex);
}

bool paintHomeIconTab(freeink::ui::DrawTarget&, freeink::ui::Rect rect, const freeink::ui::TabItem& tab, uint8_t,
                      void* userData) {
  auto* data = static_cast<HomeIconTabPainterData*>(userData);
  if (data == nullptr || data->renderer == nullptr || data->launchers == nullptr) return false;
  const int index = tab.value;
  if (index < 0 || index >= static_cast<int>(data->launcherCount)) return false;
  const auto& launcher = *data->launchers[index];
  const uint8_t* icon =
      homeTabIcon(launcher.icon == UIIcon::None ? defaultLauncherIcon(launcher.action) : launcher.icon);
  if (icon == nullptr) return false;
  data->renderer->drawIcon(icon, rect.x, rect.y, rect.width, rect.height);
  return true;
}

struct WidgetRenderEntry {
  const ThemeHomeWidgetSpec* widget;
  int actionOffset;
  size_t order;
};

struct WidgetRenderEntries {
  std::array<WidgetRenderEntry, kMaxThemeWidgets> items;
  size_t count = 0;

  void push(const WidgetRenderEntry& entry) {
    if (count >= items.size()) return;
    items[count++] = entry;
  }
};

bool themeHomeActionVisible(ThemeHomeAction action, bool hasOpdsServers, bool hasRecentBooks) {
  if (action == ThemeHomeAction::OpdsBrowser) return hasOpdsServers;
  if (action == ThemeHomeAction::RecentBook) return hasRecentBooks;
  return true;
}

WidgetRenderEntries buildRenderEntries(const ThemeHomeScreenSpec& spec, const std::vector<RecentBook>& recentBooks,
                                       bool hasOpdsServers) {
  WidgetRenderEntries entries;
  int nextActionOffset = 0;
  for (size_t i = 0; i < spec.widgets.size(); ++i) {
    const auto& widget = spec.widgets[i];
    const int widgetActionOffset = nextActionOffset;
    if (widget.type == ThemeHomeWidgetType::Recents) {
      nextActionOffset += static_cast<int>(recentBooks.size());
    } else if (widget.type == ThemeHomeWidgetType::FeaturedBookCard) {
      if (std::max(0, widget.featured.startIndex) < static_cast<int>(recentBooks.size())) ++nextActionOffset;
    } else if (widget.type == ThemeHomeWidgetType::RecentCoverGrid) {
      const int maxItems = widget.coverGrid.rows > 0 ? widget.coverGrid.rows * std::max(1, widget.coverGrid.columns)
                                                     : static_cast<int>(recentBooks.size());
      const int startIndex = std::max(0, widget.coverGrid.startIndex);
      nextActionOffset += std::min({std::max(0, static_cast<int>(recentBooks.size()) - startIndex), maxItems,
                                    static_cast<int>(kMaxThemeCoverGridItems)});
    } else if (widget.type == ThemeHomeWidgetType::LauncherList || widget.type == ThemeHomeWidgetType::LauncherGrid) {
      nextActionOffset += static_cast<int>(
          std::count_if(widget.launcher.items.begin(), widget.launcher.items.end(), [&](const auto& launcher) {
            return themeHomeActionVisible(launcher.action, hasOpdsServers, !recentBooks.empty());
          }));
    }
    entries.push(WidgetRenderEntry{&widget, widgetActionOffset, i});
  }
  std::stable_sort(entries.items.begin(), entries.items.begin() + entries.count, [](const auto& a, const auto& b) {
    if (a.widget->layer != b.widget->layer) return a.widget->layer < b.widget->layer;
    return a.order < b.order;
  });
  return entries;
}
}  // namespace

void buildThemeHomeActions(const ThemeHomeScreenSpec* spec, const std::vector<RecentBook>& recentBooks,
                           bool hasOpdsServers, std::vector<ThemeHomeActionEntry>& actions) {
  actions.clear();
  if (spec != nullptr) {
    for (const auto& widget : spec->widgets) {
      if (widget.type == ThemeHomeWidgetType::Recents) {
        for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
          actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::RecentBook, i});
        }
      } else if (widget.type == ThemeHomeWidgetType::FeaturedBookCard) {
        const int index = std::max(0, widget.featured.startIndex);
        if (index < static_cast<int>(recentBooks.size())) {
          actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::RecentBook, index});
        }
      } else if (widget.type == ThemeHomeWidgetType::RecentCoverGrid) {
        const int maxItems = widget.coverGrid.rows > 0 ? widget.coverGrid.rows * std::max(1, widget.coverGrid.columns)
                                                       : static_cast<int>(recentBooks.size());
        const int startIndex = std::max(0, widget.coverGrid.startIndex);
        for (int i = 0; startIndex + i < static_cast<int>(recentBooks.size()) && i < maxItems &&
                        i < static_cast<int>(kMaxThemeCoverGridItems);
             ++i) {
          actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::RecentBook, startIndex + i});
        }
      } else if (widget.type == ThemeHomeWidgetType::LauncherList || widget.type == ThemeHomeWidgetType::LauncherGrid) {
        for (const auto& launcher : widget.launcher.items) {
          if (themeHomeActionVisible(launcher.action, hasOpdsServers, !recentBooks.empty())) {
            actions.push_back(ThemeHomeActionEntry{launcher.action, 0});
          }
        }
      }
    }
    if (!actions.empty()) return;
  }

  for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
    actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::RecentBook, i});
  }
  actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::FileBrowser, 0});
  actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::RecentBooks, 0});
  if (hasOpdsServers) actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::OpdsBrowser, 0});
  actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::FileTransfer, 0});
  actions.push_back(ThemeHomeActionEntry{ThemeHomeAction::Settings, 0});
}

bool renderThemeHome(ThemeHomeRenderContext& ctx) {
  const auto pageWidth = ctx.renderer.getScreenWidth();
  const auto pageHeight = ctx.renderer.getScreenHeight();

  ThemeLayoutSlots slots;
  layoutThemeSlots(ctx.spec.layout, Rect{0, 0, pageWidth, pageHeight}, ctx.metrics, slots);
  if (slots.empty()) {
    const auto& layout = ctx.spec.layout;
    const auto& first = layout.children.empty() ? layout : layout.children.front();
    LOG_ERR("HOME",
            "SD home layout emitted no slots: page=%dx%d children=%d firstId=%s firstType=%d firstSize=%d firstFlex=%d",
            pageWidth, pageHeight, static_cast<int>(layout.children.size()), first.id.c_str(),
            static_cast<int>(first.sizeType), first.size, first.flex);
  }
  const bool sdHomeUsable = !slots.empty() && !ctx.actions.empty();
  if (!sdHomeUsable) {
    LOG_ERR("HOME", "Invalid SD home layout: widgets=%d slots=%d actions=%d; using built-in layout",
            static_cast<int>(ctx.spec.widgets.size()), static_cast<int>(slots.size()),
            static_cast<int>(ctx.actions.size()));
    return false;
  }

  ctx.renderer.clearScreen();
  ctx.coverRectX = 0;
  ctx.coverRectY = 0;
  ctx.coverRectW = 0;
  ctx.coverRectH = 0;

  const auto renderWidgets = buildRenderEntries(ctx.spec, ctx.recentBooks, ctx.hasOpdsServers);
  for (size_t renderIndex = 0; renderIndex < renderWidgets.count; ++renderIndex) {
    const auto& entry = renderWidgets.items[renderIndex];
    const auto& widget = *entry.widget;
    Rect slot = placedWidgetRect(findThemeSlot(slots, widget.slot), widget);
    if (slot.width <= 0 || slot.height <= 0) continue;

    if (widget.type == ThemeHomeWidgetType::Header) {
      const auto title = homeHeaderTitle(ctx.metrics, ctx.recentBooks, ctx.coverSelectorIndex);
      GUI.drawHeader(ctx.renderer, slot, title.empty() ? nullptr : title.c_str());
    } else if (widget.type == ThemeHomeWidgetType::HeaderTitle) {
      const auto title = homeHeaderTitle(ctx.metrics, ctx.recentBooks, ctx.coverSelectorIndex);
      if (!title.empty()) {
        const auto truncated = ctx.renderer.truncatedText(UI_10_FONT_ID, title.c_str(), slot.width);
        const int textWidth = ctx.renderer.getTextWidth(UI_10_FONT_ID, truncated.c_str());
        ctx.renderer.drawText(UI_10_FONT_ID, slot.x + std::max(0, (slot.width - textWidth) / 2),
                              slot.y + std::max(0, (slot.height - ctx.renderer.getLineHeight(UI_10_FONT_ID)) / 2),
                              truncated.c_str());
      }
    } else if (widget.type == ThemeHomeWidgetType::Battery) {
      const bool showBatteryPercentage =
          SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
      const int batteryX = slot.x + std::max(0, slot.width - ctx.metrics.batteryWidth);
      GUI.drawBatteryRight(ctx.renderer, Rect{batteryX, slot.y, ctx.metrics.batteryWidth, ctx.metrics.batteryHeight},
                           showBatteryPercentage);
    } else if (widget.type == ThemeHomeWidgetType::Clock) {
      if (halClock.isAvailable()) {
        char timeBuf[9];
        if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
          auto clockText = ctx.renderer.truncatedText(SMALL_FONT_ID, timeBuf, slot.width);
          const int textWidth = ctx.renderer.getTextWidth(SMALL_FONT_ID, clockText.c_str());
          ctx.renderer.drawText(SMALL_FONT_ID, slot.x + std::max(0, (slot.width - textWidth) / 2), slot.y,
                                clockText.c_str());
        }
      }
    } else if (widget.type == ThemeHomeWidgetType::Recents) {
      const bool hasCoverArea = slot.height > 0 && ctx.metrics.homeCoverHeight > 0;
      ctx.coverRectX = 0;
      ctx.coverRectY = std::max(0, slot.y - kCoverCacheBleed);
      ctx.coverRectW = pageWidth;
      ctx.coverRectH =
          std::min(pageHeight - ctx.coverRectY, slot.height + (slot.y - ctx.coverRectY) + kCoverCacheBleed);

      const bool selectorSensitiveCoverCache = GUI.homeCoverCacheDependsOnSelector();
      const bool coverStripSelected = ctx.selectorIndex >= entry.actionOffset &&
                                      ctx.selectorIndex < entry.actionOffset + static_cast<int>(ctx.recentBooks.size());
      if (coverStripSelected) {
        ctx.coverSelectorIndex = ctx.actions[ctx.selectorIndex].value;
      }
      const bool coverCacheMatches =
          !selectorSensitiveCoverCache || (ctx.coverBufferSelectorIndex == ctx.coverSelectorIndex &&
                                           ctx.coverBufferStripSelected == coverStripSelected);
      if (hasCoverArea && ctx.coverBufferStored && !coverCacheMatches) {
        ctx.coverBufferStored = false;
        ctx.coverRendered = false;
      }
      bool bufferRestored = hasCoverArea && ctx.coverBufferStored && coverCacheMatches &&
                            ctx.restoreCoverBuffer != nullptr && ctx.restoreCoverBuffer(ctx.coverBufferUserData);

      if (hasCoverArea) {
        GUI.drawRecentBookCover(
            ctx.renderer, slot, ctx.recentBooks, ctx.coverSelectorIndex, ctx.coverRendered, ctx.coverBufferStored,
            bufferRestored,
            [store = ctx.storeCoverBuffer, userData = ctx.coverBufferUserData]() {
              return store != nullptr && store(userData);
            },
            coverStripSelected);
      }
    } else if (widget.type == ThemeHomeWidgetType::FeaturedBookCard) {
      const int bookIndex = std::max(0, widget.featured.startIndex);
      if (bookIndex < static_cast<int>(ctx.recentBooks.size())) {
        const bool selected = ctx.selectorIndex >= entry.actionOffset && ctx.selectorIndex < entry.actionOffset + 1 &&
                              ctx.actions[ctx.selectorIndex].value == bookIndex;
        ctx.renderer.drawText(UI_10_FONT_ID, slot.x, slot.y, tr(STR_CONTINUE_READING), true, EpdFontFamily::BOLD);

#if FREEINK_HAVE_GFX_RENDERER
        const int labelH = ctx.renderer.getLineHeight(UI_10_FONT_ID) + std::max(0, widget.featured.titleGap);
        const int coverHeight =
            widget.featured.coverHeight > 0 ? widget.featured.coverHeight : std::max(1, slot.height - labelH - 8);
        const int coverWidth =
            widget.featured.coverWidth > 0 ? widget.featured.coverWidth : std::max(1, coverHeight * 62 / 100);
        freeink::ui::GfxRendererFrame<> ui(ctx.renderer, SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID);
        RecentBookCoverPainterData painterData{&ctx.renderer, &ctx.recentBooks,
                                               UITheme::getInstance().getHomeCoverThumbHeight(),
                                               widget.featured.placeholderIconSize};
        freeink::ui::BookCardProps props;
        props.title = ctx.recentBooks[bookIndex].title.c_str();
        props.author = ctx.recentBooks[bookIndex].author.c_str();
        props.progressMax = 0;
        props.value = static_cast<int16_t>(bookIndex);
        props.state = selected ? freeink::ui::StateSelected : freeink::ui::StateNormal;
        props.coverSize = freeink::ui::makeSize(coverWidth, coverHeight);
        props.padding = freeink::ui::makeInsets(0);
        props.gap = freeink::ui::clampI16(widget.featured.coverGap);
        props.titleText.font = freeink::ui::GfxRendererTarget::FONT_TITLE;
        props.titleText.maxLines = 2;
        props.authorText.font = freeink::ui::GfxRendererTarget::FONT_BODY;
        props.centerTextVertically = true;
        props.selectionIndicator = freeink::ui::BookCardSelectionIndicator::CoverFrame;
        props.selectedCoverFrameRadius = freeink::ui::clampRadius(widget.featured.selectedRadius);
        props.coverPainter = paintBookCardCover;
        props.coverPainterUserData = &painterData;
        freeink::ui::StyleSet styles =
            widgetSelectionStyles(ThemeWidgetSelectionStyle::Outline, widget.featured.selectedRadius);
        styles.normal.background = freeink::ui::Paint::solid(freeink::ui::Color::White);
        props.styles = styles;
        const int cardH = std::min(std::max(1, slot.height - labelH), coverHeight);
        freeink::ui::bookCard(ui.frame, freeink::ui::makeRect(slot.x, slot.y + labelH, slot.width, cardH), props);
#endif
      }
    } else if (widget.type == ThemeHomeWidgetType::RecentCoverGrid) {
      const int columns = std::max(1, widget.coverGrid.columns);
      const int rows = widget.coverGrid.rows > 0
                           ? widget.coverGrid.rows
                           : std::max(1, (static_cast<int>(ctx.recentBooks.size()) + columns - 1) / columns);
      const int startIndex = std::max(0, widget.coverGrid.startIndex);
      const int maxItems = std::min({std::max(0, static_cast<int>(ctx.recentBooks.size()) - startIndex), rows * columns,
                                     static_cast<int>(kMaxThemeCoverGridItems)});
      if (maxItems > 0) {
        const int selectedLocal =
            ctx.selectorIndex >= entry.actionOffset && ctx.selectorIndex < entry.actionOffset + maxItems
                ? ctx.actions[ctx.selectorIndex].value - startIndex
                : -1;
        const int coverHeight =
            widget.coverGrid.coverHeight > 0
                ? widget.coverGrid.coverHeight
                : std::max(1, (slot.height - std::max(0, widget.coverGrid.gap) * (rows - 1)) / rows -
                                  widget.coverGrid.labelHeight);
        const int coverWidth =
            widget.coverGrid.coverWidth > 0 ? widget.coverGrid.coverWidth : std::max(1, coverHeight * 62 / 100);
        const int rowHeight = widget.coverGrid.rowHeight > 0
                                  ? widget.coverGrid.rowHeight
                                  : coverHeight + std::max(0, widget.coverGrid.labelHeight) + 6;

#if FREEINK_HAVE_GFX_RENDERER
        freeink::ui::GfxRendererFrame<> ui(ctx.renderer, SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID);
        RecentBookCoverPainterData painterData{&ctx.renderer, &ctx.recentBooks,
                                               UITheme::getInstance().getHomeCoverThumbHeight(),
                                               widget.coverGrid.placeholderIconSize};
        HomeCoverGridItemProviderData itemProviderData{&ctx.recentBooks, startIndex};
        freeink::ui::CoverGridProps props;
        props.itemProvider = provideHomeCoverGridItem;
        props.itemProviderUserData = &itemProviderData;
        props.count = static_cast<uint16_t>(maxItems);
        props.selectedIndex = static_cast<int16_t>(selectedLocal);
        props.columns = static_cast<uint8_t>(std::min(columns, 12));
        props.gap = freeink::ui::clampI16(widget.coverGrid.gap);
        props.rowGap =
            freeink::ui::clampI16(widget.coverGrid.rowGap >= 0 ? widget.coverGrid.rowGap : widget.coverGrid.gap);
        props.cellInset = toFreeInkInsets(widget.coverGrid.cellInset);
        props.labelInset = toFreeInkInsets(widget.coverGrid.labelInset);
        props.coverSize = freeink::ui::makeSize(coverWidth, coverHeight);
        props.rowHeight = freeink::ui::clampI16(rowHeight, 1);
        props.labelHeight = freeink::ui::clampI16(widget.coverGrid.labelHeight);
        props.labelGap = freeink::ui::clampI16(widget.coverGrid.labelGap);
        props.titleText.font = freeink::ui::GfxRendererTarget::FONT_SMALL;
        props.titleText.maxLines = static_cast<uint8_t>(std::max(1, std::min(3, widget.coverGrid.labelLines)));
        props.cellStyles = widgetSelectionStyles(widget.coverGrid.selectionStyle, widget.coverGrid.selectedRadius);
        props.selectionIndicator = coverGridSelectionIndicator(widget.coverGrid.selectionStyle);
        props.selectedCoverFrameRadius = freeink::ui::clampRadius(widget.coverGrid.selectedRadius);
        props.coverPainter = paintRecentCoverGridCover;
        props.coverPainterUserData = &painterData;
        freeink::ui::coverGrid(ui.frame, freeink::ui::makeRect(slot.x, slot.y, slot.width, slot.height), props);
#endif
      }
    } else if (widget.type == ThemeHomeWidgetType::LauncherList || widget.type == ThemeHomeWidgetType::LauncherGrid) {
      std::array<const ThemeHomeLauncherSpec*, kMaxThemeLauncherItems> launchers;
      size_t launcherCount = 0;
      for (const auto& launcher : widget.launcher.items) {
        if (themeHomeActionVisible(launcher.action, ctx.hasOpdsServers, !ctx.recentBooks.empty())) {
          if (launcherCount < launchers.size()) launchers[launcherCount++] = &launcher;
        }
      }
      const int selectedLocal = ctx.selectorIndex >= entry.actionOffset &&
                                        ctx.selectorIndex < entry.actionOffset + static_cast<int>(launcherCount)
                                    ? ctx.selectorIndex - entry.actionOffset
                                    : -1;

      if (widget.launcher.presentation == ThemeLauncherPresentation::IconTabs) {
#if FREEINK_HAVE_GFX_RENDERER
        std::array<freeink::ui::TabItem, kMaxThemeLauncherItems> items;
        size_t itemCount = 0;
        for (int i = 0; i < static_cast<int>(launcherCount); ++i) {
          items[itemCount++] = freeink::ui::tabItem(i, selectedLocal == i);
        }
        freeink::ui::GfxRendererFrame<> ui(ctx.renderer, SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID);
        freeink::ui::StyleSet styles = freeink::ui::outlinedButtonStyles(widget.launcher.selectedRadius);
        HomeIconTabPainterData painterData{&ctx.renderer, launchers.data(), launcherCount};
        freeink::ui::TabBarProps props;
        props.tabs = items.data();
        props.count = static_cast<uint8_t>(std::min<size_t>(itemCount, 255));
        props.tabStyles = styles;
        props.gap = freeink::ui::clampI16(widget.launcher.gap);
        props.iconSize = freeink::ui::clampI16(widget.launcher.iconSize, 1);
        props.tabInset = freeink::ui::makeInsets(4);
        props.iconPainter = paintHomeIconTab;
        props.iconPainterUserData = &painterData;
        freeink::ui::tabBar(ui.frame, freeink::ui::makeRect(slot.x, slot.y, slot.width, slot.height), props);
#endif
      } else if (widget.type == ThemeHomeWidgetType::LauncherGrid) {
        const int columns = std::max(1, widget.launcher.columns);
        const int rows = widget.launcher.rows > 0
                             ? widget.launcher.rows
                             : std::max(1, (static_cast<int>(launcherCount) + columns - 1) / columns);
        const int gap = std::max(0, widget.launcher.gap);
        const int cellW = std::max(1, (slot.width - gap * (columns - 1)) / columns);
        const int cellH = std::max(1, (slot.height - gap * (rows - 1)) / rows);
        for (int i = 0; i < static_cast<int>(launcherCount); ++i) {
          const int col = i % columns;
          const int row = i / columns;
          if (row >= rows) break;
          Rect cell{slot.x + col * (cellW + gap), slot.y + row * (cellH + gap),
                    col == columns - 1 ? slot.x + slot.width - (slot.x + col * (cellW + gap)) : cellW, cellH};
          GUI.drawButtonMenu(
              ctx.renderer, cell, 1, selectedLocal == i ? 0 : -1,
              [&launchers, i](int) {
                return launchers[i]->text.empty() ? std::string(defaultLauncherLabel(launchers[i]->action))
                                                  : launchers[i]->text;
              },
              [&launchers, i](int) {
                return launchers[i]->icon == UIIcon::None ? defaultLauncherIcon(launchers[i]->action)
                                                          : launchers[i]->icon;
              });
        }
      } else {
        GUI.drawButtonMenu(
            ctx.renderer, slot, static_cast<int>(launcherCount), selectedLocal,
            [&launchers](int index) {
              return launchers[index]->text.empty() ? std::string(defaultLauncherLabel(launchers[index]->action))
                                                    : launchers[index]->text;
            },
            [&launchers](int index) {
              return launchers[index]->icon == UIIcon::None ? defaultLauncherIcon(launchers[index]->action)
                                                            : launchers[index]->icon;
            });
      }
    } else if (widget.type == ThemeHomeWidgetType::ButtonHints) {
      const bool horizontalBottomHints = ctx.spec.navigation == ThemeHomeNavigationMode::SplitAxis ||
                                         ctx.spec.navigation == ThemeHomeNavigationMode::CarouselAxis;
      const auto labels = ctx.mappedInput.mapLabels(
          buttonHintLabel(widget.buttonHints.back, ""), buttonHintLabel(widget.buttonHints.confirm, tr(STR_SELECT)),
          buttonHintLabel(widget.buttonHints.previous, horizontalBottomHints ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP)),
          buttonHintLabel(widget.buttonHints.next, horizontalBottomHints ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN)));
      GUI.drawButtonHints(ctx.renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  }

  ctx.renderer.displayBuffer();
  return true;
}
