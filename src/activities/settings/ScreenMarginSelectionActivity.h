#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class ScreenMarginSelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> options;

 protected:
  void loadItems() override;

 public:
  explicit ScreenMarginSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& onBack);
};
