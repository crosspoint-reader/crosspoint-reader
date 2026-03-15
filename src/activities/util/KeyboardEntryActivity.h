#pragma once
#include <GfxRenderer.h>

#include <functional>
#include <string>
#include <utility>

#include "network/RemoteKeyboardNetworkSession.h"

#include "../Activity.h"

/**
 * Reusable keyboard entry activity for text input.
 * Can be started from any activity that needs text entry via startActivityForResult()
 */
class KeyboardEntryActivity : public Activity {
 public:
  /**
   * Constructor
   * @param renderer Reference to the GfxRenderer for drawing
   * @param mappedInput Reference to MappedInputManager for handling input
   * @param title Title to display above the keyboard
   * @param initialText Initial text to show in the input field
   * @param maxLength Maximum length of input text (0 for unlimited)
   * @param isPassword If true, display asterisks instead of actual characters
   * @param startY Y position to start rendering the keyboard
   */
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, const bool isPassword = false, const int startY = 10)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        isPassword(isPassword),
        startY(startY) {}

  // Activity overrides
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override;
  bool blocksBackgroundServer() override;

 private:
  enum class InputMode { Remote, Local };

  std::string title;
  std::string text;
  size_t maxLength;
  bool isPassword;
  int startY;
  InputMode inputMode = InputMode::Local;
  uint32_t remoteSessionId = 0;
  std::unique_ptr<RemoteKeyboardNetworkSession> remoteNetworkSession;
  unsigned long lastRemoteRefreshAt = 0;

  // Keyboard state
  int selectedRow = 0;
  int selectedCol = 0;
  bool shiftActive = false;

  // Handlers
  void onComplete(std::string text);
  void onCancel();

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

  char getSelectedChar() const;
  bool handleKeyPress();  // false if onComplete was triggered
  int getRowLength(int row) const;
  void renderItemWithSelector(int x, int y, const char* item, bool isSelected) const;
  void switchToLocalInput();
  void renderRemoteMode(RenderLock&&);
};
