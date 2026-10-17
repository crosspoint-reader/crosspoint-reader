#pragma once

#include <cstdint>
#include <string>

#include "CrossPointState.h"
#include "activities/UiListActivity.h"

namespace BookFinished {
std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex);

// Moves the book and its sidecar files into /COMPLETED; outMovedPath gets its new path. Its reading
// state follows it (BookRelocation): at once, or with `openInReader` once that reader has closed it.
bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath, bool openInReader);

// Applies the remove-from-recents and move-to-/COMPLETED settings to a finished book; returns the
// path it has afterwards.
std::string applyFinishedBookSettings(const std::string& bookPath, bool openInReader);

enum class FinishedBookAction {
  Stay = 0,
  GoHome = 1,
  OpenNextBook = 2,
  SearchOpdsForAuthor = 3,
};

// Launches the finished-book menu on top of `host` and handles its result:
// credits a finish to the reading-stats session, applies the move-to-/COMPLETED
// and remove-from-recents settings, then navigates home or to the next book.
// On cancel/stay the host gets a requestUpdate() to re-render its last page.
// The caller is responsible for persisting reading progress beforehand (the
// progress formats differ per reader).
// `onMenuClosed` (optional, with `onMenuClosedCtx`) runs first in the result
// handler regardless of outcome — readers use it to clear a "menu is open"
// flag. Plain function pointer + context instead of std::function per the
// project callback convention.
// `onSyncToKOReader` (optional, with `onSyncToKOReaderCtx`), when non-null, offers a "sync
// progress to KOReader" toggle (only when KOReader credentials are also configured) alongside
// the move-to-/COMPLETED and forget-book toggles. Unlike those two, applying it isn't a plain
// settings write: it's invoked in place of navigating away directly, with the book's current
// path (which may already be the moved-to-/COMPLETED path — the move/forget settings are still
// applied first, "in parallel" rather than replaced), a KOReaderSyncPostAction describing which
// of Go Home / Open Next / Search OPDS was picked, and that action's target (next-book path or
// OPDS author, empty for Go Home). The callback owns pushing progress and, once its own reboot
// completes, performing that action itself (EpubReaderActivity is the only caller that supplies
// one, since KOReader sync is EPUB-only).
// It returns whether it actually took over: the sync path REPLACES this flow's own navigation, so
// a callback that declines (no credentials, no live Epub to read a position from) must say so or
// the user is left sitting on a dead screen with nothing having happened. False means "I did not
// navigate", and the picked action is performed here as if the toggle had been off.
void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            const std::string& author = {}, void (*onMenuClosed)(void*) = nullptr,
                            void* onMenuClosedCtx = nullptr,
                            bool (*onSyncToKOReader)(void*, const std::string& bookPath, KOReaderSyncPostAction,
                                                     const std::string& target) = nullptr,
                            void* onSyncToKOReaderCtx = nullptr);
}  // namespace BookFinished

// What to do now that the book is finished. Pushed by a reader when the last page is turned (see
// launchFinishedBookFlow) and by the file browser's "mark as read". The rows are actions (Home,
// open the next book, search OPDS for the author) and switches (move to /COMPLETED, forget the
// book, sync to KOReader) that the result handler applies once an action is picked. Above the
// list sit the header, two lines of text and a preview of the next book.
class FinishedBookActivity final : public UiListActivity {
 public:
  FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string currentBookPath,
                       std::string nextBookPath, std::string currentBookAuthor = {},
                       bool koReaderSyncAvailable = false);

  void onEnter() override;

 private:
  // The rows that can appear, in display order. Which ones do depends on the book and the settings;
  // rebuildRows() keeps those that apply in actions_.
  enum class Row : uint8_t { GoHome, OpenNext, SearchOpds, ToggleMoveToCompleted, ToggleForget, ToggleSyncToKOReader };
  static constexpr int kMaxRows = 6;

  int listCount() const override { return actionCount_; }
  const char* headerTitle() const override;
  void drawChrome() override;
  void buildScreen(UiScreen& screen) override;
  void afterUiRender() override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  // Back means "done with this book": a GoHome result, which credits the finish and applies the
  // switches. It never cancels.
  void onBackButton() override;
  // Long Back is Back here, also when no reader is below (the file browser's "mark as read"),
  // where going Home directly would skip the switches.
  void homeFromList() override;

  void rebuildRows();
  void loadNextBookPreview();
  int layoutNextBookPreview(const Rect& content, int top);
  void finishWith(BookFinished::FinishedBookAction action);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  std::string currentBookPath_;
  std::string nextBookPath_;
  std::string currentBookAuthor_;
  // Read by the render task (the next-book row and the panel above the list); written on the loop
  // task, under RenderLock, once the preview has loaded.
  std::string nextBookTitle_;
  std::string nextBookAuthor_;
  std::string nextBookSeries_;
  std::string nextBookCoverPath_;
  int coverWidth_ = 0;  // the cover bitmap's own size; 0 = no cover to show
  int coverHeight_ = 0;
  // "Search OPDS for author: <author>", built with the rows.
  std::string opdsLabel_;
  Row actions_[kMaxRows]{};
  uint8_t actionCount_ = 0;
  // Laid out by drawChrome() on every pass and read in the same frame: the list's top edge by
  // buildScreen(), the cover's place by afterUiRender().
  int listTop_ = 0;
  int coverX_ = 0;
  int coverY_ = 0;
  int coverW_ = 0;
  int coverH_ = 0;
  bool nextBookAvailable_ = false;
  bool nextBookMetadataLoaded_ = false;
  bool koReaderSyncAvailable_ = false;
};
