#pragma once
#include <GfxRenderer.h>
#include <InputManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"

/**
 * Reusable keyboard entry activity for text input.
 * Can be started from any activity that needs text entry.
 *
 * Usage:
 *   1. Create a KeyboardEntryActivity instance
 *   2. Set callbacks with setOnComplete() and setOnCancel()
 *   3. Call onEnter() to start the activity
 *   4. Call loop() in your main loop
 *   5. When complete or cancelled, callbacks will be invoked
 */
class KeyboardEntryActivity : public Activity {
 public:
  // Callback types
  using OnCompleteCallback = std::function<void(const std::string&)>;
  using OnCancelCallback = std::function<void()>;

  /**
   * Constructor
   * @param renderer Reference to the GfxRenderer for drawing
   * @param inputManager Reference to InputManager for handling input
   * @param title Title to display above the keyboard
   * @param initialText Initial text to show in the input field
   * @param startY Y position to start rendering the keyboard
   * @param maxLength Maximum length of input text (0 for unlimited)
   * @param isPassword If true, display asterisks instead of actual characters
   * @param onComplete Callback invoked when input is complete
   * @param onCancel Callback invoked when input is cancelled
   */
  explicit KeyboardEntryActivity(GfxRenderer& renderer, InputManager& inputManager, std::string title = "Enter Text",
                                 std::string initialText = "", const int startY = 10, const size_t maxLength = 0,
                                 const bool isPassword = false, OnCompleteCallback onComplete = nullptr,
                                 OnCancelCallback onCancel = nullptr)
      : Activity("KeyboardEntry", renderer, inputManager),
        title(std::move(title)),
        text(std::move(initialText)),
        startY(startY),
        maxLength(maxLength),
        isPassword(isPassword),
        onComplete(std::move(onComplete)),
        onCancel(std::move(onCancel)) {}

  // Activity overrides
  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string title;
  int startY;
  std::string text;
  size_t maxLength;
  bool isPassword;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  // Keyboard state
  int selectedRow = 0;
  int selectedCol = 0;
  bool shiftActive = false;

  // Callbacks
  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;

  // Keyboard layout
  static constexpr int NUM_ROWS = 5;
  static constexpr int KEYS_PER_ROW = 13;  // Max keys per row (rows 0 and 1 have 13 keys)
  static const char* const keyboard[NUM_ROWS];
  static const char* const keyboardShift[NUM_ROWS];

  // Special key positions (bottom row)
  static constexpr int SPECIAL_ROW = 4;
  static constexpr int SHIFT_COL = 0;
  static constexpr int SPACE_COL = 2;
  static constexpr int BACKSPACE_COL = 7;
  static constexpr int DONE_COL = 9;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  char getSelectedChar() const;
  void handleKeyPress();
  int getRowLength(int row) const;
  void render() const;
  void renderItemWithSelector(int x, int y, const char* item, bool isSelected) const;
};
