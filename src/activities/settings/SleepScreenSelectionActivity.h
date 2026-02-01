#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class SleepScreenSelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> options;

 protected:
  void loadItems() override;

 public:
  explicit SleepScreenSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& onBack);
};
