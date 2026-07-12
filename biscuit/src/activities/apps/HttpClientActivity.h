#pragma once
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HttpClientActivity final : public Activity {
 public:
  explicit HttpClientActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HttpClient", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MENU, ENTER_URL, ENTER_BODY, CONNECTING, RESULT };
  enum Method { GET, POST };

  State state = MENU;
  Method method = GET;
  int menuIndex = 0;
  ButtonNavigator buttonNavigator;

  std::string url;
  std::string postBody;
  std::string responseBody;
  int responseCode = 0;
  unsigned long responseTimeMs = 0;

  int scrollOffset = 0;
  int totalLines = 0;
  static constexpr int MAX_RESPONSE_BYTES = 4096;

  void performRequest();
  int getMenuItemCount() const;
};
