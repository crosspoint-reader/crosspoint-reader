#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "activities/UiListActivity.h"

class HyphenationManagerActivity final : public UiListActivity {
 public:
  HyphenationManagerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* wantedCode = nullptr);
  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override { return loading_; }

 private:
  static constexpr size_t MAX_PACKS = 48;
  struct Pack {
    char code[3] = {};
    char name[40] = {};
    uint32_t size = 0;
    uint32_t fileCrc = 0;
    uint32_t payloadCrc = 0;
    bool supported = false;
    bool builtIn = false;
  };

  std::array<Pack, MAX_PACKS> packs_{};
  std::unique_ptr<freeink::ui::ListItem[]> rows_;
  std::string baseUrl_;
  char wantedCode_[3] = {};
  char failedCode_[3] = {};
  size_t packCount_ = 0;
  bool loading_ = false;
  bool wifiStarted_ = false;
  bool manifestFailed_ = false;

  int listCount() const override { return loading_ ? 0 : static_cast<int>(packCount_); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawChrome() override;
  void scanSdRoot();
  void loadOfflineList();
  void onWifiReady(bool connected);
  bool loadManifest();
  bool downloadPack(Pack& pack);
  bool fileCrcMatches(const char* path, uint32_t expected);
  void confirmRemoval(const Pack& pack);
  void chooseUpdateOrRemoval(Pack& pack);
};
