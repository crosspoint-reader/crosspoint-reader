#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

struct WifiResult { bool connected = false; std::string ssid; std::string ip; };
struct KeyboardResult { std::string text; };
struct MenuResult { int action = -1; uint8_t orientation = 0; uint8_t pageTurnOption = 0; };
struct ChapterResult { int spineIndex = 0; };
struct PercentResult { int percent = 0; };
struct PageResult { uint32_t page = 0; };
struct SyncResult { int spineIndex = 0; int page = 0; };
struct FootnoteResult { std::string href; };
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };
struct NetworkModeResult { NetworkMode mode; };

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult,
    ChapterResult, PercentResult, PageResult, SyncResult, NetworkModeResult, FootnoteResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;
  ActivityResult() = default;
  template <typename T> ActivityResult(T&& r) : data{std::forward<T>(r)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
