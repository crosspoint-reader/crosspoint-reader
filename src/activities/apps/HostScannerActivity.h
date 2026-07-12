#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HostScannerActivity final : public Activity {
 public:
  explicit HostScannerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HostScanner", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { CHECKING_WIFI, SCANNING_HOSTS, HOST_LIST, PORT_SCANNING, PORT_RESULTS };

  struct Host {
    uint32_t ip;
    std::vector<uint16_t> openPorts;
  };

  State state = CHECKING_WIFI;
  std::vector<Host> hosts;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int scanProgress = 0;
  int scanTotal = 254;
  int portScanProgress = 0;
  int selectedHost = -1;

  static constexpr uint16_t COMMON_PORTS[] = {21, 22, 23, 25, 53, 80, 139, 443, 445, 993, 3389, 5900, 8080, 8443};
  static constexpr int NUM_PORTS = 14;

  void startHostScan();
  void scanNextHost();
  void startPortScan(int hostIndex);
  void scanNextPort();
  void saveToCsv();
  std::string ipToString(uint32_t ip) const;
};
