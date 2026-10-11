#pragma once
#include <OpdsParser.h>

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "../ListController.h"
#include "OpdsBackTo.h"
#include "OpdsServerStore.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    BOOK_DETAIL,
    FORMAT_SELECTION,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server,
                                   OpdsBackTo backTo, std::string initialQuery = {})
      : Activity("OpdsBookBrowser", renderer, mappedInput),
        server(std::move(server)),
        backTo(backTo),
        initialQuery_(std::move(initialQuery)) {}

  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  // Tap on a row -> move the selection there; ActivityManager synthesizes Confirm. Serves
  // BOTH of this screen's lists (the entry list and the format picker), keyed on state.
  ListRowTap::Result selectListRow(int index) override;
  // A swipe pages whichever list is on screen; it never reaches Left/Right (Search and Info).
  bool pageList(ListPageDirection direction) override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  BrowserState state = BrowserState::LOADING;
  std::vector<uint32_t> entryOffsets;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool memoryTrimmed = false;
  bool coverAvailable = false;
  int selectorIndex = 0;
  int selectedBookIndex = -1;
  int formatSelectorIndex = 0;
  std::vector<std::string> formatSelectionLabels;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing
  OpdsBackTo backTo;
  // Set by leave() on its way to File Transfer, so onExit()'s reboot lands there and not on Home.
  bool leavingToFileTransfer = false;
  std::string initialQuery_;

  OpdsEntry getEntry(size_t index) const;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  // Back from the top of the catalog: to backTo.
  void leave();
  void downloadBook(const OpdsEntry& book, const OpdsAcquisitionLink& acquisition);
  void chooseBookFormat(const OpdsEntry& book);
  void fetchOsdTemplate(const std::string& osdUrl);
  void launchSearch();
  void performSearch(const std::string& query);
  void fetchCoverForEntry(const OpdsEntry& entry);
  bool preventAutoSleep() override;

  int catalogRowsPerPage() const;
  // Rows per page as the last render drew them. Paging runs on the loop task, where the renderer's
  // orientation can be mid-flip to Portrait for a hint strip, so input reads what render published.
  std::atomic<int> catalogPageRows{1};
  std::atomic<int> formatPageRows{1};
  // The top row of the page the last render drew, published for listWindow().
  std::atomic<int> catalogTop{0};
  std::atomic<int> formatTop{0};
  void showBookDetail(const OpdsEntry& entry);
  void closeFormatPicker();

  // Short Left/Right on the catalog: Search where the feed offers one, Info on a book, and never a
  // step (an action that does not apply leaves its side doing nothing). The format picker keeps
  // the default pair.
  static constexpr ListDeclaration kCatalogDeclaration{{true, StrId::STR_SEARCH}, {true, StrId::STR_INFO}};
  static constexpr ListDeclaration kFormatDeclaration{};

  struct CatalogHost final : ListHost {
    explicit CatalogHost(OpdsBookBrowserActivity& browser) : browser(browser) {}
    int listCount() const override;
    ListWindow listWindow() const override;
    bool listPagesAligned() const override { return true; }
    int listSelected() const override;
    void listSelect(int row) override;
    bool listActionAvailable(ListGrammar::Side side, int row) const override;
    void onListActivate(int row, bool longPress) override;
    void onListBack() override;
    void onListHome() override;
    void onListAction(ListGrammar::Side side, int row) override;
    OpdsBookBrowserActivity& browser;
  };

  struct FormatHost final : ListHost {
    explicit FormatHost(OpdsBookBrowserActivity& browser) : browser(browser) {}
    int listCount() const override;
    ListWindow listWindow() const override;
    bool listPagesAligned() const override { return true; }
    int listSelected() const override;
    void listSelect(int row) override;
    void onListActivate(int row, bool longPress) override;
    void onListBack() override;
    void onListHome() override;
    OpdsBookBrowserActivity& browser;
  };

  // Each controller below holds a reference to its host.
  CatalogHost catalogHost{*this};
  FormatHost formatHost{*this};
  ListController catalogList{mappedInput, buttonEvents, catalogHost, kCatalogDeclaration};
  ListController formatList{mappedInput, buttonEvents, formatHost, kFormatDeclaration};
};
