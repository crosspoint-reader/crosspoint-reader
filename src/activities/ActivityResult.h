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
  int8_t textAntiAliasingOverride = -1;
  int8_t hyphenationOverride = -1;
  int8_t fontSizeNormalizationOverride = -1;
  // File browser display options carried back from FileContextMenuActivity.
  // Appended at the end so positional MenuResult initialisers elsewhere
  // (e.g. EpubReaderMenuActivity) are unaffected.
  uint8_t sortMode = 0;
  uint8_t sortDirection = 0;
  uint8_t showHiddenFiles = 0;
  uint8_t showFileExtensions = 0;
  // Also appended for positional-initialiser safety. Tri-state like the other
  // reader overrides: -1 = default, 0 = off, 1 = on.
  int8_t guideDotsOverride = -1;
  int8_t inlineFootnotePreviewsOverride = -1;
  // Browse Files view (CrossPointSettings::FILE_BROWSER_VIEW), appended like the above.
  uint8_t browserView = 0;
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
  uint32_t visibleOffset = 0;
  bool hasVisibleOffset = false;  // content offset in the spine (exact page via the section LUT)
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

// Asks the word-selection overlay to look the same word up in another
// dictionary. The viewer does not own a Dictionary and should not: the overlay
// above it already has one open, and it is the one that knows the word.
struct DictionarySwitchResult {
  int8_t direction = 1;  // +1 = next in the discovered list, -1 = previous
};

// A full-text search hit: chapter and visible-text offset range of the match.
struct SearchResult {
  int spineIndex = 0;
  uint32_t visibleOffset = 0;
  uint32_t visibleEnd = 0;
};

struct StarredPageResult {
  int spineIndex = 0;
  int pageNumber = 0;
};

// Which answer ConfirmationActivity was given, when it was not Cancel. Only a question with an
// alternate answer needs to look: for a yes/no question isCancelled already says it all.
struct ConfirmResult {
  bool alternate = false;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, PageResult,
                 SyncResult, NetworkModeResult, FootnoteResult, FilePathResult, StarredPageResult, PrintedPageResult,
                 DictionarySwitchResult, ConfirmResult, SearchResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType, typename = std::enable_if_t<std::is_constructible_v<ResultVariant, ResultType&&>>>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
