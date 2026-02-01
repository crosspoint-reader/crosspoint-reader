#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class RefreshFrequencySelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> options;

 protected:
  void loadItems() override;

 public:
  explicit RefreshFrequencySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::function<void()>& onBack);
};
