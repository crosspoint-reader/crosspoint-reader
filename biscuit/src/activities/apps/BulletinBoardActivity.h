#pragma once
#include <WebServer.h>
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BulletinBoardActivity final : public Activity {
 public:
  explicit BulletinBoardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BulletinBoard", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == ACTIVE; }
  bool skipLoopDelay() override { return state == ACTIVE; }

 private:
  enum State { CONFIG, ACTIVE, DONE };
  State state = CONFIG;
  ButtonNavigator buttonNavigator;
  int configIndex = 0;

  // Config
  int timerMinutes = 10;
  static constexpr int TIMER_OPTIONS[] = {5, 10, 30, 60};
  static constexpr int TIMER_COUNT = 4;
  int timerIndex = 1;

  // Active state
  unsigned long startTime = 0;
  WebServer* server = nullptr;
  std::string ssidName;
  int finalPostCount = 0;  // saved before posts.clear() in stopBoard()

  // Posts stored in RAM (volatile, ephemeral)
  struct Post { char text[201]; unsigned long timestamp; };
  std::vector<Post> posts;
  static constexpr int MAX_POSTS = 50;

  void startBoard();
  void stopBoard();
  void setupWebServer();

  void renderConfig() const;
  void renderActive() const;
  void renderDone() const;

  static BulletinBoardActivity* activeInstance;
};
