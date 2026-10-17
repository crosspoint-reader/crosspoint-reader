#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>

enum class KOReaderSyncIntentState : uint8_t {
  COMPARE = 0,
  PULL_REMOTE = 1,
  PUSH_LOCAL = 2,
  // Auto variants compare progress before writing and skip silently when the other side
  // is already ahead. AUTO_PUSH fires from the reader-close auto-sync path; AUTO_PULL fires
  // when the user opens a book with long-press Confirm. Neither prompts the user.
  AUTO_PUSH = 3,
  AUTO_PULL = 4,
};

enum class KOReaderSyncOutcomeState : uint8_t {
  NONE = 0,
  PENDING = 1,
  CANCELLED = 2,
  FAILED = 3,
  UPLOAD_COMPLETE = 4,
  APPLIED_REMOTE = 5,
};

// Where KOReaderSyncActivity lands once a sync completes (or fails/cancels) and the device has
// rebooted to reclaim WiFi-session heap fragmentation. Reader/Home cover the two cases that
// existed before the finished-book "sync + then continue with the picked action" flow; OpenBook
// and OpdsSearch let that flow land on the next book / an OPDS search after syncing, instead of
// only ever the synced book or Home.
enum class KOReaderSyncPostAction : uint8_t {
  Reader = 0,      // reopen APP_STATE.openEpubPath (the book that was being synced)
  Home = 1,        // go to the home screen
  OpenBook = 2,    // reopen a different book (postActionTarget = its path)
  OpdsSearch = 3,  // go home, then launch an OPDS author search (postActionTarget = author)
};

struct PendingBookmarkJumpState {
  bool active = false;
  std::string bookPath;     // source file path for disambiguation
  uint16_t spineIndex = 0;  // EPUB spine; ignored for TXT
  uint16_t pageNumber = 0;  // page within spine (EPUB) or global page (TXT)

  void clear() {
    active = false;
    bookPath.clear();
    spineIndex = 0;
    pageNumber = 0;
  }
};

#if SERIAL_TEST_API
// Where the next EPUB open should land, as a share of the book (the serial
// CMD:ACT:READER:<path>@<n>%). RAM only: unlike a bookmark jump it is acted on by the open that
// follows it at once, so there is nothing to survive a reboot for.
struct PendingPercentJumpState {
  bool active = false;
  std::string bookPath;
  uint8_t percent = 0;
};
#endif

struct KOReaderSyncSessionState {
  bool active = false;
  std::string epubPath;
  int spineIndex = 0;
  int page = 0;
  int totalPagesInSpine = 0;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  // The paragraph LUT index at the end of the page BEFORE `page` (0 on the first page). With
  // paragraphIndex it bounds the paragraphs that open on `page`, which is how the sync screen
  // tells "same page" from "ahead" without the section cache (ProgressComparison).
  uint16_t paragraphIndexBefore = 0;
  // The page's start and the end of its window as content offsets (Section::getVisibleTextOffsetForPage
  // and getVisibleTextOffsetAfterPage), for the sync screen to push the page exactly and to compare a
  // mapped record's offset against [visibleOffsetAtPage, visibleOffsetAtNextPage). hasVisibleOffset
  // false when the chapter has no LUT.
  uint32_t visibleOffsetAtPage = 0;
  uint32_t visibleOffsetAtNextPage = UINT32_MAX;
  bool hasVisibleOffset = false;
  KOReaderSyncIntentState intent = KOReaderSyncIntentState::COMPARE;
  KOReaderSyncOutcomeState outcome = KOReaderSyncOutcomeState::NONE;
  int resultSpineIndex = 0;
  int resultPage = 0;
  uint16_t resultParagraphIndex = 0;
  bool resultHasParagraphIndex = false;
  // Running <li> count for the matched element, used by EpubReaderActivity to snap a
  // KOReader-supplied list-item XPath to the precise page via Section::getPageForListItemIndex.
  // Preferred over resultParagraphIndex when the deepest target element is /li[N].
  uint16_t resultListItemIndex = 0;
  bool resultHasListItemIndex = false;
  // The remote position as a visible-text offset in resultSpineIndex (only for a KOReader text
  // point matched to the codepoint), which EpubReaderActivity turns into the exact page via
  // Section::getPageForVisibleTextOffset. Preferred over both LUT indices above.
  uint32_t resultVisibleOffset = 0;
  bool resultHasVisibleOffset = false;
  // Where to land once this sync (and its reboot) completes. Defaults to Reader so auto-push-on-
  // close and reader-menu-triggered syncs keep their existing behavior without every call site
  // having to set it explicitly; AUTO_PUSH's caller sets it to Home, the finished-book flow sets
  // it to whichever of Home / OpenBook / OpdsSearch matches the action the user picked.
  KOReaderSyncPostAction postAction = KOReaderSyncPostAction::Reader;
  // Book path (OpenBook) or author (OpdsSearch); unused for Reader/Home.
  std::string postActionTarget;
  // Set by RecentBooks / FileBrowser long-press to ask the reader to perform an AUTO_PULL
  // before rendering its first page. Stored by EPUB path so the flag cannot leak across books.
  std::string autoPullEpubPath;

  // Every result* field: where an applied remote position lands. One list, so a new result field
  // cannot be missed by one of the sites that reset them.
  void clearResult() {
    resultSpineIndex = 0;
    resultPage = 0;
    resultParagraphIndex = 0;
    resultHasParagraphIndex = false;
    resultListItemIndex = 0;
    resultHasListItemIndex = false;
    resultVisibleOffset = 0;
    resultHasVisibleOffset = false;
  }

  void clear() {
    active = false;
    epubPath.clear();
    spineIndex = 0;
    page = 0;
    totalPagesInSpine = 0;
    paragraphIndex = 0;
    hasParagraphIndex = false;
    paragraphIndexBefore = 0;
    visibleOffsetAtPage = 0;
    visibleOffsetAtNextPage = UINT32_MAX;
    hasVisibleOffset = false;
    intent = KOReaderSyncIntentState::COMPARE;
    outcome = KOReaderSyncOutcomeState::NONE;
    clearResult();
    postAction = KOReaderSyncPostAction::Reader;
    postActionTarget.clear();
    autoPullEpubPath.clear();
  }
};

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  std::string openEpubPath;
  size_t lastSleepImage = SIZE_MAX;  // SIZE_MAX = unset sentinel
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  // How Recent Books shows its list: a CrossPointSettings::FILE_BROWSER_VIEW, chosen there from
  // Options > View. Its own, not Browse Files' (SETTINGS.fileBrowserView): changing one leaves the
  // other as it was. Covers by default, as the grid was the screen's own view.
  uint8_t recentBooksView = 2;
  // The Library tab last left (a LibraryTab), reopened from Home.
  uint8_t libraryTab = 0;
  // How New and Authors show their lists, each chosen there from Options > View: New as covers, like
  // Recent Books; Authors in Details, its rows being names.
  uint8_t addedBooksView = 2;
  uint8_t authorsView = 1;
  // When false, setup() skips the boot screen on wake and instead restores the
  // saved framebuffer overlaid with a loading icon (Quick Resume).
  bool showBootScreen = true;
  KOReaderSyncSessionState koReaderSyncSession;
  PendingBookmarkJumpState pendingBookmarkJump;
#if SERIAL_TEST_API
  PendingPercentJumpState pendingPercentJump;  // not saved; see the type
#endif
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
