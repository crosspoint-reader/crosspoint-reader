#pragma once

#include <functional>
#include <string>

#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// FreeInkUI chrome for the toolbar reader menu (Settings -> Reader -> Reader
// Menu Style -> Toolbar), painted over the page that is already on screen:
//
//  - Toolbar: a top header (book title, battery) and a bottom sheet holding a
//    chapter scrub row (< capsule >) and a Contents / Text / More tile row.
//  - Panel: a taller bottom sheet with a title, a paged row list (name left,
//    value right) and the same tile row as a switcher.
//
// EpubReaderActivity owns the state (which overlay, selection, row contents)
// and the physical-button handling; this class owns the layout, the tap
// targets FreeInkUI registers for it, and the translation of dispatched
// actions back into the small event set the reader acts on. Nothing here
// clears the screen: the page stays visible around the chrome.
class ReaderToolbarUi : public UiAppHost {
 public:
  enum class Event { None, Dismiss, Tool, PrevChapter, NextChapter, Scrub, Row };

  struct Model {
    bool panel = false;  // false = toolbar, true = a Contents/Text/More panel
    // Toolbar
    const char* bookTitle = nullptr;
    const char* chapterTitle = nullptr;
    const char* pageInfo = nullptr;  // "12/40   51%"
    int progressPermille = 0;        // 0..1000 book progress (scrub handle)
    // Panel
    const char* panelTitle = nullptr;
    int itemCount = 0;
    int selectedIndex = -1;  // row the buttons' cursor sits on; -1 = none shown
    std::function<std::string(int)> rowText;
    std::function<std::string(int)> rowValue;
    // Tile row: the tool in focus (toolbar) / the open panel (panel). 0..2.
    int activeTool = 0;
    // Pixels kept free along the screen's bottom edge under the panel sheet
    // (the button-hint row on boards without touch). 0 on touch boards.
    int bottomReserve = 0;
    // Button boards keep the theme's denser list row height (as every other
    // list does there); touch boards use FreeInkUI's finger-sized rows.
    bool denseRows = false;
  };

  struct Routed {
    Event event = Event::None;
    int value = 0;        // Tool: tool index; Row: row index
    int permille = -1;    // Scrub: 0..1000 along the track
    bool routed = false;  // the gate was open and a touch frame was routed
    int x = 0;            // touch position of the routed frame (logical px)
    int y = 0;
  };

  explicit ReaderToolbarUi(GfxRenderer& renderer);

  // Screen-entry reset + action wiring. Call once when the overlay opens.
  void begin();
  // What the next render() draws. The strings must stay alive until render()
  // returns; the row callbacks are invoked during render() for the visible rows.
  void setModel(const Model& model) { model_ = model; }
  // Paint into the framebuffer (no refresh, no clear). Run from the render task.
  void render();
  // Route one loop-task input frame; returns the action it mapped to, if any.
  Routed route(const MappedInputManager& input);

  // Panel list viewport after the last render: rows that fit one page, and the
  // first row shown. 0 before the first render.
  int visibleRows() const { return visibleRows_; }
  int topIndex() const { return topIndex_; }

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void buildToolbar(UiScreen& screen);
  void buildPanel(UiScreen& screen);
  void buildToolRow(UiScreen& screen, freeink::ui::LayoutAnchor anchor);
  void buildHeader(UiScreen& screen);
  int16_t toolRowHeight(const UiScreen& screen) const;

  Model model_;
  Routed pending_;
  int visibleRows_ = 0;
  int topIndex_ = 0;

  // Row window materialised for the visible page only (a contents list runs to
  // hundreds of entries; labels are copied here so ListItem can point at them).
  static constexpr int kMaxWindow = 16;
  std::string windowLabels_[kMaxWindow];
  std::string windowValues_[kMaxWindow];
  freeink::ui::ListItem windowItems_[kMaxWindow];
  // fui::ButtonProps / ListProps / HeaderProps embed a 324-byte StyleSet: keep
  // them off the stack (AGENTS.md: locals stay under 256 bytes).
  freeink::ui::ButtonProps stepProps_;
  freeink::ui::ListProps listProps_;
  freeink::ui::HeaderProps headerProps_;
  char batteryText_[8] = {};
  freeink::ui::Rect pageIndicatorRect_{};
};
