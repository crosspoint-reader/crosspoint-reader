#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MdnsBrowserActivity final : public Activity {
 public:
  explicit MdnsBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("MdnsBrowser", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == BROWSING; }

 private:
  enum State { CHECK_WIFI, SERVICE_SELECT, BROWSING, RESULTS, DETAIL };
  State state = CHECK_WIFI;

  struct ServiceResult {
    std::string hostname;
    std::string ip;
    uint16_t port;
    std::string serviceType;
    std::string instanceName;
  };

  struct ServiceType {
    const char* type;
    const char* label;
    const char* description;
  };
  static constexpr int NUM_SERVICES = 10;
  static const ServiceType SERVICE_TYPES[NUM_SERVICES];

  std::vector<ServiceResult> results;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int detailIndex = 0;
  int serviceIndex = 0;

  int scanServiceIdx = 0;
  bool scanAllServices = false;
  bool mdnsStarted = false;

  void startDiscovery(int serviceIdx);
  void startDiscoveryAll();
  void queryService(const char* serviceType);
  void saveToCsv();
};
