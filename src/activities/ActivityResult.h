#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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
  int nameId = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
  int8_t embeddedStyleOverride = -1;
  int8_t imageRenderingOverride = -1;
  int8_t fontFamilyOverride = -1;
  std::string sdFontFamilyOverride;
  int8_t fontSizeOverride = -1;
  uint8_t textDarkness = 1;
  uint8_t bionicReadingOverride = 0;
  int8_t paragraphAlignmentOverride = -1;
};

struct ChapterResult {
  int spineIndex = 0;
  std::optional<int> tocIndex;
};

struct PercentResult {
  int percent = 0;
};

struct PrintedPageResult {
  std::string label;
};

struct PageResult {
  uint32_t page = 0;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;                    // estimated page (fallback)
  uint16_t paragraphIndex = 0;     // 1-based <p> index from XPath
  bool hasParagraphIndex = false;  // true when paragraphIndex is available
  uint16_t listItemIndex = 0;      // running <li> count when XPath ends in /li[N]
  bool hasListItemIndex = false;   // true when listItemIndex is available
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

struct StarredPageResult {
  int spineIndex = 0;
  int pageNumber = 0;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, PageResult,
                 SyncResult, NetworkModeResult, FootnoteResult, FilePathResult, StarredPageResult, PrintedPageResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType, typename = std::enable_if_t<std::is_constructible_v<ResultVariant, ResultType&&>>>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
