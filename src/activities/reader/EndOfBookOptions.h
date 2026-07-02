#pragma once

#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Shared End-of-Book "next book" state for the EPUB and XTC readers (SETTINGS.endOfBookBehavior).
// Collects up to MAX_SUGGESTIONS sibling books once per reader session, handles the Ask-mode
// list input, and draws the end screen for every mode.
class EndOfBookOptions {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage, Menu };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  // Scans the book's folder for suggestions; no-op when already loaded or when the
  // configured behavior is plain go-home. Call from loop() once the end screen is reached
  // so the scan never delays a regular page turn.
  void loadOnce(const std::string& currentBookPath);

  // True when the Ask-mode list is showing and should own the reader's input.
  bool askMenuActive() const;

  // Full path of the first suggestion (Next book mode); empty when there is none.
  std::string firstSuggestionPath() const;

  // Ask-mode input handling. Front Left/Right move the selection, Confirm and side
  // page-forward open it (or Home), side page-back returns to the last page, and a short
  // Back press asks for the reader's menu. Fills openPath when the result is OpenBook.
  // Returns Action::None when nothing relevant was pressed; callers continue their normal
  // input path (keeping long-press Back to the file browser working).
  Action handleAskInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen (title plus mode-specific extras) onto a cleared buffer.
  // menuLabel is the hint for the Back button in Ask mode (pass "" when the reader has
  // no menu to open, e.g. an XTC without chapters).
  void render(GfxRenderer& renderer, const MappedInputManager& input, const char* menuLabel) const;

 private:
  std::string folder;
  std::vector<std::string> names;
  int selector = 0;
  bool isLoaded = false;

  std::string fullPath(size_t index) const;
};
