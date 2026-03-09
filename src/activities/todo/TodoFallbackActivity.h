#pragma once

#include <string>

#include "../Activity.h"

class TodoFallbackActivity final : public Activity {
  std::string dateText;
  void* onBackCtx;
  void (*onBack)(void*);

  void render() const;

 public:
  explicit TodoFallbackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string dateText,
                                void* onBackCtx, void (*onBack)(void*))
      : Activity("TodoFallback", renderer, mappedInput),
        dateText(std::move(dateText)),
        onBackCtx(onBackCtx),
        onBack(onBack) {}

  void onEnter() override;
  void loop() override;
};
