#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "activities/reader/PerBookReaderSettings.h"

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t autoPageTurnSeconds = 0;
  bool autoPageTurnChanged = false;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct ReaderSettingsResult {
  PerBookReaderSettings settings;
};

struct ClippingSelectionResult {
  std::string text;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t wordCount = 0;
  // Exact identity of startPage. A multi-page selection may safely reopen at
  // its start without pretending that one CRC identifies every later page.
  uint32_t pageFingerprint = 0;
};

struct ClippingJumpResult {
  // Complete in-memory identity of the store entry selected by the user. The
  // reader must re-open the store and compare every field before changing the
  // reading position; none of these values is a trusted navigation command on
  // its own.
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  std::string storePath;
  std::string chapterTitle;
  uint32_t storeFileLength = 0;
  uint8_t storeFormat = UINT8_MAX;
  uint16_t clippingIndex = 0;
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  uint32_t textCrc32 = 0;
  // Exact identity of startPage; later pages are never matched by guessing.
  uint32_t pageFingerprint = 0;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult,
                                   IntervalResult, PageResult, ProgressChangeResult, NetworkModeResult, FootnoteResult,
                                   FilePathResult, ReaderSettingsResult, ClippingSelectionResult, ClippingJumpResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
