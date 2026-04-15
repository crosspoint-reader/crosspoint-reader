#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>

enum class KOReaderSyncIntentState : uint8_t {
  COMPARE = 0,
  PULL_REMOTE = 1,
  PUSH_LOCAL = 2,
};

enum class KOReaderSyncOutcomeState : uint8_t {
  NONE = 0,
  PENDING = 1,
  CANCELLED = 2,
  FAILED = 3,
  UPLOAD_COMPLETE = 4,
  APPLIED_REMOTE = 5,
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

struct KOReaderSyncSessionState {
  bool active = false;
  std::string epubPath;
  int spineIndex = 0;
  int page = 0;
  int totalPagesInSpine = 0;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  KOReaderSyncIntentState intent = KOReaderSyncIntentState::COMPARE;
  KOReaderSyncOutcomeState outcome = KOReaderSyncOutcomeState::NONE;
  int resultSpineIndex = 0;
  int resultPage = 0;
  uint16_t resultParagraphIndex = 0;
  bool resultHasParagraphIndex = false;

  void clear() {
    active = false;
    epubPath.clear();
    spineIndex = 0;
    page = 0;
    totalPagesInSpine = 0;
    paragraphIndex = 0;
    hasParagraphIndex = false;
    intent = KOReaderSyncIntentState::COMPARE;
    outcome = KOReaderSyncOutcomeState::NONE;
    resultSpineIndex = 0;
    resultPage = 0;
    resultParagraphIndex = 0;
    resultHasParagraphIndex = false;
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
  KOReaderSyncSessionState koReaderSyncSession;
  PendingBookmarkJumpState pendingBookmarkJump;
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
