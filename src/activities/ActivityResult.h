#pragma once

#include <cstdint>
#include <functional>
#include <string>

// TODO: move this to the correct place
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

struct ActivityResult {
  // IMPORTANT: only include copiable data here, do NOT pass a pointer or reference

  bool isCancelled = false;

  // For NetworkModeSelectionActivity result
  NetworkMode selectedNetworkMode = NetworkMode::JOIN_NETWORK;

  // For WifiSelectionActivity result
  std::string wifiSSID;
  std::string wifiIP;
  bool wifiConnected = false;

  // For EpubReaderMenuActivity result
  uint8_t selectedOrientation = 0;

  // For KeyboardEntryActivity result
  std::string inputText;

  // For XtcReaderChapterSelectionActivity result
  uint32_t selectedPage = 0;

  // For KOReaderSyncActivity result
  int syncedSpineIndex = 0;
  int syncedPage = 0;

  // For EpubReaderMenuActivity result (-1 = back/cancelled, else cast of MenuAction)
  int menuAction = -1;

  // For EpubReaderChapterSelectionActivity result
  int selectedSpineIndex = 0;

  // For EpubReaderPercentSelectionActivity result
  int selectedPercent = 0;
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
