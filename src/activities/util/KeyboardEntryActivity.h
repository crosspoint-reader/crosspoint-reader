#pragma once
#include <GfxRenderer.h>

#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

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
   * @param mappedInput Reference to MappedInputManager for handling input
   * @param title Title to display above the keyboard
   * @param initialText Initial text to show in the input field
   * @param maxLength Maximum length of input text (0 for unlimited)
   * @param isPassword If true, display asterisks instead of actual characters
   * @param onComplete Callback invoked when input is complete
   * @param onCancel Callback invoked when input is cancelled
   */
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, const bool isPassword = false,
                                 OnCompleteCallback onComplete = nullptr, OnCancelCallback onCancel = nullptr)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        isPassword(isPassword),
        onComplete(std::move(onComplete)),
        onCancel(std::move(onCancel)) {}

  // Activity overrides
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  bool isPassword;

  ButtonNavigator buttonNavigator;

  // Keyboard state
  int selectedRow = 0;
  int selectedCol = 0;
  int shiftState = 0;  // 0 = lower case, 1 = upper case, 2 = shift lock)

  // Callbacks
  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;

  // Keyboard layout
  static constexpr int NUM_ROWS = 5;
  static constexpr int KEYS_PER_ROW = 13;  // Max keys per row (rows 0 and 1 have 13 keys)
  static const char* const keyboard[NUM_ROWS];
  static const char* const keyboardShift[NUM_ROWS];
  static const char* const shiftString[3];

  // Special key positions (bottom row)
  static constexpr int SPECIAL_ROW = 4;
  static constexpr int SHIFT_COL = 0;
  static constexpr int SPACE_COL = 2;
  static constexpr int BACKSPACE_COL = 7;
  static constexpr int DONE_COL = 9;

  char getSelectedChar() const;
  void handleKeyPress();
  int getRowLength(int row) const;
};
