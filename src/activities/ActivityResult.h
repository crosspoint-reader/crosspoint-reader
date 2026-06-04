#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
  uint8_t pageTurnOption = 0;
};

struct ChapterResult {
  int spineIndex = 0;
};

struct PercentResult {
  int percent = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;
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

struct ClippingResult {
  std::string text;
  int fromWordIdx = -1;
  int toWordIdx = -1;
  uint16_t sectionPage = 0;
  uint16_t endSectionPage = 0;
  std::string startText;
  std::string endText;
  std::string beforeStartText;
  std::string afterEndText;
  std::string midText;
  uint16_t wordCount = 0;
};

/// Returned by ClipSelectionActivity in Mode::WORD_SELECT.
/// Contains only the selected text and word indices — no anchor metadata.
struct WordSelectResult {
  std::string text;
  int fromWordIdx = -1;
  int toWordIdx = -1;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, PageResult,
                 SyncResult, NetworkModeResult, FootnoteResult, FilePathResult, ClippingResult, WordSelectResult>;

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
