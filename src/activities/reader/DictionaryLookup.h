#pragma once

#include <DictionaryStore.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"

// Button-driven word selection + definition popup for the reader.
//
// Lifecycle: enter() takes ownership of the freshly loaded Page (word text is
// arena-backed and only valid while the Page lives), builds word bounding
// boxes and highlights the first word. handleInput() then owns all input:
// Left/Right/side buttons move between words, Up/Down move between lines,
// Confirm looks the word up in the first .cpd dictionary found under
// /dictionaries on the SD card, Back leaves the mode.
//
// Drawing is overlay-only: the pixels under the highlight/popup are snapshot
// with copyRegionToBuffer and restored on close, so no page re-render is
// needed. If a snapshot allocation fails the overlay is drawn anyway and the
// caller is asked for a full re-render on exit (EXITED_NEEDS_RENDER).
class DictionaryLookup {
 public:
  enum class TickResult : uint8_t {
    NONE,                 // still active
    EXITED,               // left the mode, screen already restored
    EXITED_NEEDS_RENDER,  // left the mode, caller must re-render the page
  };

  DictionaryLookup(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput) {}

  // Takes the current page and enters selection mode (draws the first
  // highlight). Returns false if the page has no words.
  bool enter(std::unique_ptr<Page> page, int marginLeft, int marginTop, int fontId);
  bool active() const { return state != State::INACTIVE; }
  TickResult handleInput();
  // Drops all state without touching the screen (used when a full re-render
  // supersedes the overlay, and from onExit()).
  void reset();

 private:
  enum class State : uint8_t { INACTIVE, SELECTING, POPUP, MESSAGE };

  struct WordBox {
    const char* text;  // arena-backed, owned by `page`
    int16_t x, y;      // top-left, logical screen coordinates
    int16_t w, h;
    uint16_t lineIndex;
  };

  static constexpr int HIGHLIGHT_PADDING = 2;
  static constexpr int HIGHLIGHT_LINE_WIDTH = 2;
  static constexpr uint8_t GHOST_CLEAN_EVERY_MOVES = 8;
  static constexpr int POPUP_MARGIN = 16;
  static constexpr int POPUP_PADDING = 12;
  static constexpr int MAX_DEFINITION_LINES = 200;
  static constexpr const char* DICTIONARY_DIR = "/dictionaries";

  void buildWordBoxes(int marginLeft, int marginTop);
  void moveSelection(int wordDelta, int lineDelta);
  void drawHighlight();
  void eraseHighlight();
  void lookupSelectedWord();
  void openPopup(const char* header, const std::string& body);
  void showMessage(StrId messageId);
  void drawPopupFrame(int popupX, int popupY, int popupW, int popupH);
  void drawPopupContents();
  // Returns false when the page under the popup could not be restored; the
  // caller must then leave the mode and request a full re-render.
  bool closePopup();
  bool ensureDictionaryOpen();
  // Snapshot helpers; snapshot() returns false when the region could not be
  // saved (OOM), in which case restore() is a no-op and fullRenderNeeded is set.
  bool snapshotRegion(int x, int y, int w, int h, std::unique_ptr<uint8_t[]>& buf);
  void restoreRegion(int x, int y, int w, int h, std::unique_ptr<uint8_t[]>& buf);

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  State state = State::INACTIVE;
  std::unique_ptr<Page> page;
  std::vector<WordBox> words;
  int selected = 0;
  int fontId = 0;
  bool fullRenderNeeded = false;
  uint8_t movesSinceGhostClean = 0;

  // Saved pixels under the current highlight / popup
  std::unique_ptr<uint8_t[]> highlightSnapshot;
  int highlightRect[4] = {0, 0, 0, 0};
  std::unique_ptr<uint8_t[]> popupSnapshot;
  int popupRect[4] = {0, 0, 0, 0};

  // Popup contents
  std::string popupHeader;
  std::vector<std::string> popupLines;
  int popupScroll = 0;
  int popupVisibleLines = 0;

  // Dictionary (kept open across lookups; file must outlive the store)
  HalFile dictFile;
  DictionaryStore dictStore;
  bool dictOpenFailed = false;
};
