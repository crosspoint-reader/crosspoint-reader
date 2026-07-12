#pragma once

#include <atomic>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, handles the menu input, and
// draws the end screen. With no suggestions the end screen keeps its historical
// plain-title look and behavior.
class EndOfBookOptions {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  // Scans the book's folder for suggestions; no-op when already loaded. Call ONLY from
  // the reader's render() (the render task, serialized by RenderLock) — the loaded flag
  // is the release/acquire publication point that lets the main task read the finished
  // list safely.
  void loadOnce(const std::string& currentBookPath);

  // True when the suggestion menu is showing and should own the reader's input.
  bool menuActive() const;

  // Menu input handling, following the standard list idiom: side Up/Down and front
  // Left/Right move the selection (wrapping), Confirm opens it (or Home), and a short
  // Back press returns to the last page of the book. Fills openPath when the result is
  // OpenBook. Returns Action::None when nothing relevant was pressed; callers continue
  // their normal input path (keeping long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen (plain title, or the suggestion menu) onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input) const;

 private:
  std::string folder;
  // Written by the render task in loadOnce(), immutable afterwards; the main task only
  // reads it after isLoaded is observed true (acquire), so no further locking is needed.
  std::vector<std::string> names;
  int selector = 0;
  std::atomic<bool> isLoaded{false};

  std::string fullPath(size_t index) const;
};
