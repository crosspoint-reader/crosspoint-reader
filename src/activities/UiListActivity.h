#pragma once

#include <atomic>
#include <functional>

#include "activities/Activity.h"
#include "activities/ListController.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

enum class SettingAction;
struct Rect;

class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  // Stops touch routing before the activity goes away. Every screen here builds its UI in
  // onEnter() (resetUi() re-arms routing), so closing it on the way out is always right --
  // and doing it here means a subclass cannot forget. Two did it by hand; the rest did not.
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // What a tap on a row means. Overriding it here is what puts the FreeInkUI path and the
  // legacy ListTouchBand path on the same rule: onRowAction() used to apply ListRowTap
  // inline, so a subclass could override this (as MenuListActivity does, to decline
  // separators) and never be consulted for a tap it actually receives.
  ListRowTap::Result selectListRow(int index) override;

 protected:
  bool tryOpenSliderFor(SettingAction action, std::function<void()> onDone = {});

  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  // `declaration` is the screen's Left/Right pair and long-press shape (ListController.h); the
  // default is the plain list the pickers and menus use.
  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 const ListDeclaration& declaration = {});

  virtual int listCount() const = 0;
  virtual void buildScreen(UiScreen& screen) = 0;
  virtual void activateIndex(int index) = 0;
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  virtual bool handleCustomInput() { return false; }
  virtual void onBackButton() { finish(); }
  virtual void onSelectionChanged(int /*index*/) {}
  // False for rows the selection must step over, such as a section separator. Default: all rows.
  [[nodiscard]] virtual bool isRowSelectable(int /*index*/) const { return true; }
  // Runs once the UI has been laid out and drawn, before the footer and the
  // buffer swap. For anything that has to patch the framebuffer using a
  // rectangle only the layout pass knows -- see FontSelectionActivity, which
  // blits a cached preview strip here.
  virtual void afterUiRender() {}
  virtual int indexForActionValue(int16_t value) const { return value; }
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  virtual void drawFooter();
  // What drawFooter() puts on Back and Confirm. Left/Right and the side boxes come from the list's
  // declaration, through the controller.
  [[nodiscard]] virtual const char* footerBackLabel() const;
  [[nodiscard]] virtual const char* footerConfirmLabel() const;
  // Reads this list's buttons through the controller. The file browser overrides it with its own
  // until it moves to the controller (PR 6).
  virtual void navigateButtons();

  // --- The list as the button scheme sees it: one position per row, unless a subclass puts
  // something in front (TabbedUiListActivity's tab bar is position 0). ---
  [[nodiscard]] virtual int positionCount() const { return listCount(); }
  [[nodiscard]] virtual int selectedPosition() const;
  virtual void selectPosition(int position) { moveSelectionTo(position); }
  virtual void showPositionPage(int position, int topPosition) { showRowPage(position, topPosition); }
  [[nodiscard]] virtual ListWindow positionWindow() const { return publishedWindow(); }
  // Positions in front of row 0 that stay above the rows rather than scroll with them (a tab bar).
  [[nodiscard]] virtual int leadPositions() const { return 0; }
  [[nodiscard]] virtual bool isPositionSelectable(int position) const { return isRowSelectable(position); }
  virtual void activatePosition(int position, bool longPress);
  virtual void backFromPosition(int /*position*/) { onBackButton(); }
  // Long Back. Home, except while a book is open below this screen: there it is Back, so the
  // reader's own close path (overrides, KOSync push) is never skipped. A screen whose way out does
  // more than leave overrides it.
  virtual void homeFromList();
  virtual void switchTab(int /*direction*/) {}

  // The content area a list lays out in: room for the bottom hints AND the side Up/Down boxes.
  [[nodiscard]] Rect listContentRect() const;
  // The theme's header, placed inside listContentRect() so the side hint strip never sits over it.
  [[nodiscard]] Rect listHeaderRect() const;
  // Both hint strips for this list, with footerBackLabel() / footerConfirmLabel() on Back / Confirm.
  void drawListHints();
  // Call once a render pass has laid the list out: hands the drawn window to the input side.
  void publishListWindow();
  [[nodiscard]] ListWindow publishedWindow() const;
  // Forget the window the last render published, so a list that changes under it (another tab)
  // does not page by the previous one's row count until its own first render.
  void resetPublishedWindow();

  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  // The list's area below the header: screen margins from listContentRect() (both hint strips
  // reserved) plus the theme's top padding and header height, then the theme's spacer. extraTop /
  // extraBottom make room for a screen's own panel above the list or band below it. `spacer` false
  // leaves out the theme's spacer, for a bar the caller puts directly under the header.
  void layoutListArea(UiScreen& screen, int16_t extraTop = 0, int16_t extraBottom = 0, bool spacer = true);
  // The props every list here starts from: rows act on tap, touch input, body-text labels up to two
  // lines. The caller sets items / count / rowProvider and anything it draws differently.
  [[nodiscard]] freeink::ui::ListProps listProps(UiScreen& screen) const;
  // Syncs the viewport (subtitle rows are taller on button-only boards) and adds the list.
  void addList(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  // Tells the serial test interface which row `props` highlights and its label (CMD:STATE sel=,
  // label=). addList() does it; a screen that calls screen.list() itself calls this just before.
#if SERIAL_TEST_API
  static void noteListSelection(const freeink::ui::ListProps& props);
#else
  static void noteListSelection(const freeink::ui::ListProps&) {}
#endif
  void moveSelectionTo(int index);
  // A page turn: select `row` with the screen starting at `top` (ListGrammar::page() decides both).
  void showRowPage(int row, int top);

  freeink::ui::ListNav nav;
  // Used only by the file browser's own navigateButtons() until it moves to the controller (PR 6).
  ButtonNavigator buttonNavigator;

 private:
  // The ListController's view of this activity: it forwards to the position hooks above.
  struct ControllerHost final : ListHost {
    explicit ControllerHost(UiListActivity& list) : list(list) {}
    int listCount() const override { return list.positionCount(); }
    ListWindow listWindow() const override { return list.positionWindow(); }
    int listLeadPositions() const override { return list.leadPositions(); }
    bool listSelectable(const int position) const override { return list.isPositionSelectable(position); }
    int listSelected() const override { return list.selectedPosition(); }
    void listSelect(const int position) override { list.selectPosition(position); }
    void listShowPage(const int position, const int top) override { list.showPositionPage(position, top); }
    void onListActivate(const int position, const bool longPress) override {
      list.activatePosition(position, longPress);
    }
    void onListBack() override { list.backFromPosition(list.selectedPosition()); }
    void onListHome() override { list.homeFromList(); }
    void onListTab(const int direction) override { list.switchTab(direction); }
    UiListActivity& list;
  };

  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void onRowAction(int index);
  bool routeListTouch();

  // The window the last render drew (top row, rows drawn), for paging from the loop task.
  std::atomic<int> windowTop{0};
  std::atomic<int> windowDrawn{1};
  ControllerHost controllerHost{*this};
  ListController listController;
};
