#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "Activity.h"

/**
 * ListSelectionActivity is a reusable base class for activities that display
 * a scrollable list of items with selection capabilities.
 *
 * Features:
 * - Automatic pagination based on screen size
 * - Page skipping when holding navigation buttons
 * - Configurable title, empty message, and button labels
 * - Customizable item rendering
 */
class ListSelectionActivity : public Activity {
 protected:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  size_t selectorIndex = 0;
  bool updateRequired = false;
  unsigned long enterTime = 0;
  
  // Configuration
  std::string title;
  std::string emptyMessage;
  std::string backLabel;
  std::string confirmLabel;
  std::function<size_t()> getItemCount;
  std::function<std::string(size_t)> getItemText;
  std::function<void(size_t)> onItemSelected;
  std::function<void()> onBack;
  std::function<void(size_t, int, int, bool)> customRenderItem;  // index, x, y, isSelected
  
  // Constants
  static constexpr int SKIP_PAGE_MS = 700;
  static constexpr unsigned long IGNORE_INPUT_MS = 300;
  static constexpr int LINE_HEIGHT = 30;
  static constexpr int START_Y = 60;
  static constexpr int BOTTOM_BAR_HEIGHT = 60;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  int getPageItems() const;
  virtual void loadItems() {}  // Override to load items on enter

 public:
  explicit ListSelectionActivity(const std::string& activityName, GfxRenderer& renderer,
                                MappedInputManager& mappedInput, const std::string& title,
                                std::function<size_t()> getItemCount,
                                std::function<std::string(size_t)> getItemText,
                                std::function<void(size_t)> onItemSelected,
                                std::function<void()> onBack,
                                const std::string& emptyMessage = "No items available",
                                const std::string& backLabel = "« Back",
                                const std::string& confirmLabel = "Select")
      : Activity(activityName, renderer, mappedInput),
        title(title),
        emptyMessage(emptyMessage),
        backLabel(backLabel),
        confirmLabel(confirmLabel),
        getItemCount(getItemCount),
        getItemText(getItemText),
        onItemSelected(onItemSelected),
        onBack(onBack) {}
  
  virtual ~ListSelectionActivity() = default;
  
  void onEnter() override;
  void onExit() override;
  void loop() override;
  
  // Allow subclasses to set initial selection
  void setInitialSelection(size_t index) { selectorIndex = index; }
  size_t getCurrentSelection() const { return selectorIndex; }
  
  // Allow custom item rendering
  void setCustomItemRenderer(std::function<void(size_t, int, int, bool)> renderer) {
    customRenderItem = renderer;
  }
};
