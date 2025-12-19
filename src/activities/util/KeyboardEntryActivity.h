#pragma once
#include <GfxRenderer.h>
#include <InputManager.h>

#include <functional>
#include <string>

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
   * @param maxLength Maximum length of input text (0 for unlimited)
   * @param isPassword If true, display asterisks instead of actual characters
   */
  KeyboardEntryActivity(GfxRenderer& renderer, InputManager& inputManager, const std::string& title = "Enter Text",
                        const std::string& initialText = "", size_t maxLength = 0, bool isPassword = false);

  /**
   * Handle button input. Call this in your main loop.
   * @return true if input was handled, false otherwise
   */
  bool handleInput();

  /**
   * Render the keyboard at the specified Y position.
   * @param startY Y-coordinate where keyboard rendering starts (default 10)
   */
  void render(int startY = 10) const;

  /**
   * Get the current text entered by the user.
   */
  const std::string& getText() const { return text; }

  /**
   * Set the current text.
   */
  void setText(const std::string& newText);

  /**
   * Check if the user has completed text entry (pressed OK on Done).
   */
  bool isComplete() const { return complete; }

  /**
   * Check if the user has cancelled text entry.
   */
  bool isCancelled() const { return cancelled; }

  /**
   * Reset the keyboard state for reuse.
   */
  void reset(const std::string& newTitle = "", const std::string& newInitialText = "");

  /**
   * Set callback for when input is complete.
   */
  void setOnComplete(OnCompleteCallback callback) { onComplete = callback; }

  /**
   * Set callback for when input is cancelled.
   */
  void setOnCancel(OnCancelCallback callback) { onCancel = callback; }

  // Activity overrides
  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  bool isPassword;

  // Keyboard state
  int selectedRow = 0;
  int selectedCol = 0;
  bool shiftActive = false;
  bool complete = false;
  bool cancelled = false;

  // Callbacks
  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;

  // Keyboard layout
  static constexpr int NUM_ROWS = 5;
  static constexpr int KEYS_PER_ROW = 13;  // Max keys per row (rows 0 and 1 have 13 keys)
  static const char* const keyboard[NUM_ROWS];
  static const char* const keyboardShift[NUM_ROWS];

  // Special key positions (bottom row)
  static constexpr int SHIFT_ROW = 4;
  static constexpr int SHIFT_COL = 0;
  static constexpr int SPACE_ROW = 4;
  static constexpr int SPACE_COL = 2;
  static constexpr int BACKSPACE_ROW = 4;
  static constexpr int BACKSPACE_COL = 7;
  static constexpr int DONE_ROW = 4;
  static constexpr int DONE_COL = 9;

  char getSelectedChar() const;
  void handleKeyPress();
  int getRowLength(int row) const;
};
