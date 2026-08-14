#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "components/UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, handles the menu input, and
// draws the end screen. With no suggestions the end screen keeps its historical
// plain-title look and behavior.
class EndOfBookOptions : private UiAppHost {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Scans the book's folder for suggestions; no-op when already loaded. Call ONLY from
  // the reader's render() (the render task, serialized by RenderLock) — the loaded flag
  // is the release/acquire publication point that lets the main task read the finished
  // list safely.
  void loadOnce(const std::string& currentBookPath);

  // True when the suggestion menu is showing and should own the reader's input.
  bool menuActive() const;

  // Menu input handling, following the standard list idiom: a tap on a row opens it
  // (or Home), side Up/Down and front Left/Right move the selection (wrapping),
  // Confirm opens the selection, and a short Back press returns to the last page of
  // the book. Fills openPath when the result is OpenBook. Returns Action::None when
  // nothing relevant was pressed; callers continue their normal input path (keeping
  // long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen (plain title, or the suggestion menu) onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input);

 private:
  // The UiAppHost base hosts the suggestion list (themed rows, touch routing);
  // the title and button hints stay on the legacy UITheme calls.
  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);

  GfxRenderer& renderer;
  std::string folder;
  // Written by the render task in loadOnce(), immutable afterwards; the main task only
  // reads it after isLoaded is observed true (acquire), so no further locking is needed.
  std::vector<std::string> names;
  int selector = 0;
  std::atomic<bool> isLoaded{false};

  // Row storage, built once in loadOnce() (same acquire/release publication
  // point as names — see isLoaded above) rather than per-render in
  // buildListScreen(): names.size() is capped at MAX_SUGGESTIONS and never
  // changes afterward, so a fixed-capacity array avoids any heap allocation
  // for the row list, both at load time and every subsequent repaint.
  static constexpr size_t MAX_ROWS = MAX_SUGGESTIONS + 1;  // + the trailing "Home" row
  std::string rowLabels[MAX_ROWS];
  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  size_t rowCount = 0;
  void buildRowItems();

  // Row index dispatched by onRowEvent during the current route() call; -1 otherwise.
  int tappedRow = -1;

  std::string fullPath(size_t index) const;
};
